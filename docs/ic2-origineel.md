# De originele IC2 laten zitten (`avr_versie: "org"`)

**Status: experimenteel, nog niet op een echte IC2 gedraaid.** Het berichtformaat is afgeleid uit de gedisassembleerde
firmware en een emulator ([RAPPORT.md](../reverse-engineering/analyse-volledig/RAPPORT.md) sectie 3.3), nooit op de draad
bevestigd. De component compileert; de rest is nog te testen.

Voor wie geen AVR kan programmeren: de originele ATmega8 ("Haddon T1.1") blijft in de IC2-voet en doet zelf het
OpenTherm-gesprek met thermostaat en ketel, met de interfaceschakeling die al op de print zit. De ESP32 in de IC1-voet
speelt de rol van de originele IC1 (M1.1) en praat met hem over de bestaande link. Geen extra bedrading, niets branden.

## Opstelling

- ESP32 in de IC1-voet, **originele ATmega8 in de IC2-voet**.
- Dipswitch 2 op **ON** (PC2 laag = OpenTherm-ketel). Staat hij op OFF, dan speelt de IC2 zelf ketel en praat hij niet
  met jouw ketel ([meetresultaten-ic2.md](meetresultaten-ic2.md)).
- In `aurea-wp.yaml`: `avr_versie: "org"`. De yaml heeft dezelfde uart-pinnen als v1 en v2 (GPIO19 en GPIO18).

## De link

9615 baud, 8 databits, **2 stopbits**, geen pariteit. Een frame is 12 bytes; byte 11 is de som van byte 0 t/m 10.

| Richting | Inhoud |
|---|---|
| IC2 -> ESP | byte 0 ketelsetpoint (ID 1, hele graden), byte 1-2 kamersetpoint (ID 16, hele graden en fractie, 0 = geen vraag), byte 5-10 de zes laatst van ons ontvangen bytes terug |
| ESP -> IC2 | byte 0 `0xF0` = warmtepomp niet beschikbaar, anders echo; byte 6 stand; byte 7 aanvoer (wordt ID 25); byte 8 retour (ID 28); byte 9 buitentemperatuur (door de IC2 weggegooid); byte 10 foutcode |

De IC2 zendt alleen als wij 5 s of langer stil zijn geweest, en antwoordt op ons frame niet opnieuw. Daarom
antwoordt de component één keer per IC2-frame (na 150 ms), en stuurt hij alleen uit zichzelf een frame als de IC2
30 s stil is. Hoort de IC2 240 s niets van ons, dan zet hij de noodbrug om (K1 en K2): de ketel gaat dan rechtstreeks
met de thermostaat verder.

## Wat wel en niet kan

| | `org` | v2 |
|---|---|---|
| Ketelsetpoint en kamersetpoint van de thermostaat lezen | ja | ja |
| Aanvoer en retour van de warmtepomp aan de thermostaat tonen | ja | ja |
| Ketel tegenhouden (rem) | alles of niets | met een instelbaar setpoint |
| Bijstook op een gekozen ketelsetpoint | nee (de IC2 schrijft zelf 10 graden) | ja |
| Kamertemperatuur van de thermostaat | nee, via HA of de HTTP-ingang | ja |
| Ketelgegevens (aanvoer, retour, druk, modulatie, vlam, storing) | nee | ja |
| Buitentemperatuur naar de thermostaat | nee (de IC2 gooit hem weg) | ja |
| CV uitzetten, maximale modulatie | nee | ja |
| Elk Data-ID zien | nee | ja |
| Eigen firmware branden | **niet nodig** | ja |
| Noodbrug bij wegvallen ESP32 | ja, 240 s | ja |

Entiteiten die niets kunnen tonen "onbekend". "Keteltype (dipswitch)" meldt "Niet uit te lezen". "Thermostaat
verbonden" volgt hier de link zelf: de IC2 meldt niet of er een thermostaat meepraat.

**Rem aan** (de warmtepomp heeft het gezag) wordt gestuurd als stand van minstens 1, ook als de compressor
stilstaat, anders geeft de IC2 de ketel vrij. **Rem uit** is byte 0 = `0xF0`: de IC2 schakelt naar boiler-modus en
geeft alles ongewijzigd door aan de ketel. Zolang de ESP32 nog niets heeft gestuurd (opstart) staat de rem uit, dus de
ketel werkt gewoon.

## Eerste test: alleen luisteren

```yaml
substitutions:
  avr_versie: "org"
  org_alleen_luisteren: "true"
```

De ESP stuurt dan niets. De IC2 hoort dan niets van een IC1 en zet na 240 s de noodbrug om (K1 en K2: thermostaat
rechtstreeks aan de ketel), dus houd de luistertest kort. In de log verschijnen regels
`IC2 -> ESP: ...` met alle 12 bytes. Controleer:

1. Er komen frames binnen en de som klopt ("Brug gemiste berichten" blijft 0).
2. Byte 0 volgt het ketelsetpoint van de thermostaat, byte 1 de kamerinstelling.

Pas daarna `org_alleen_luisteren` weghalen. Dan geeft **"IC2 echo klopt"** (onder Diagnose) het bewijs dat ook de
richting ESP32 -> IC2 werkt: de IC2 geeft onze bytes 5 t/m 10 terug in zijn volgende frame.

## Wat nog niet bevestigd is

- Het frame op de draad (alleen emulator en disassemblage).
- Dat de IC2 op ons antwoord na 150 ms reageert en niet in een stilteperiode zit.
- Of de Honeywell Chronotherm Touch met de originele IC2 werkt. Die werkte bij de eigen AVR pas nadat de TX-polariteit
  van PD4 was omgekeerd; de originele firmware laat PD4 in rust hoog, dus het zou moeten, maar is ongemeten.
- Wat de IC2 stuurt als er geen thermostaat is (zijn beginwaarde voor het kamersetpoint is 20 graden).
