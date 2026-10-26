# Reverse-engineering van de originele controlbox (Haddon M1.1 en T1.1)

Analyse van de firmware van de twee ATmega8-chips in de controlbox van de Atlantic Aurea 5 hybrid
(Chofu AEYC-0643XU): **M1.1** (IC1, praat 666 baud met de buitenunit) en **T1.1** (IC2, de OpenTherm-brug).

| Bestand | Inhoud |
|---|---|
| [`index.html`](index.html) | verslag: hardware gemeten, topologie, M1.1 en T1.1 per onderdeel, telegrammen, vergelijking met `chofu_wp`. Begint met een lijst correcties. |
| [`analyse-volledig/RAPPORT.md`](analyse-volledig/RAPPORT.md) | tweede, volledige ronde (26-10-2026): dekking van alle flash-bytes, functies en pinnen, de volledige telegramketen, emulatie, en de onopgeloste punten |
| `Haddon_M1.1_reversed.ino`, `Haddon_T1.1_reversed.ino` | oude C-reconstructies (deels onzeker, met een errata-blok bovenaan); het rapport is leidend |
| `analyse-volledig/*.py` | de analyse- en emulatiescripts (AVR-decoder, dekking, emulator en experimenten) |
| `analyse-volledig/*.csv`, `*.md`, `*.txt` | dekkingskaart, functielijsten, aanroepgraaf, IO- en RAM-kruisverwijzingen, telegramtabellen |

## Wat hier niet staat

De ruwe flash-dumps en de volledige disassembly-listings zijn **niet** gepubliceerd: ze bevatten de firmware
van de fabrikant. Wie zelf een dump uit zijn eigen chips leest, kan de listings en dekking met de scripts opnieuw
laten maken (`coverage.py`, `static_analysis.py`, `analyze.py`).

## Belangrijkste uitkomsten

- Alle 8192 flash-bytes per chip zijn verklaard; round-trip byte voor byte gelijk met twee assemblers.
- De link M1.1 naar T1.1 is 12 bytes plus 8-bits som op 9615 baud 8N2, zonder vaste kop.
- De firmware rekent nergens een CRC; zenden gaat uit tabellen met vooraf berekende checksums, ontvangen controleert
  alleen het ID.
- Twee extra geldige telegrammen in de flash (op 0x0baa en 0x0bb4) zijn bewezen onbereikbaar.
- `B2 02` in ID3 is een vaste constante. Een "pompniveau" bestaat in de firmware niet.
- Onopgelost: het einde van een 0x91-bericht, klok en fuses, het doel van PB3 en de betekenis van de foutcodes.

Meer in [`analyse-volledig/RAPPORT.md`](analyse-volledig/RAPPORT.md), sectie 8.
