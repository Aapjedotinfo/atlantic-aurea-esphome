# De ESP32 bijwerken zonder Home Assistant

Je hebt geen Home Assistant nodig om de ESP32 te bouwen en te flashen. Alles kan met ESPHome op je eigen pc
(Windows, macOS of Linux). Home Assistant gebruikt zelf ook gewoon ESPHome.

**Een online site die je YAML compileert bestaat niet.** De firmware moet altijd ergens gebouwd worden met
ESPHome. [ESPHome Web](https://web.esphome.io) kan alleen een kant-en-klaar `.bin`-bestand via USB in de ESP32 zetten
(zie C). Dat bestand bouw je dus eerst zelf met A of B.

Na de eerste installatie gaan updates via wifi (OTA): de ESP32 hoeft niet uit de controlbox.

## Voorbereiding (eenmalig)

```bash
git clone https://github.com/Aapjedotinfo/atlantic-aurea-esphome
cd atlantic-aurea-esphome
cp secrets.yaml.example secrets.yaml
```

- Vul in `secrets.yaml` je wifi in, een `fallback_password` en een `api_encryption`-sleutel (32 willekeurige bytes in
  base64; een sleutel maken kan op <https://esphome.io/components/api.html>).
- Zet bovenin `aurea-wp.yaml` de `avr_versie` op `"1"`, `"2"` of `"org"` (zie de README, *Kies je opzet*).
- Zonder Home Assistant mag de `api:` gewoon blijven staan: `reboot_timeout: 0s` zorgt dat de ESP32 niet herstart als
  er niemand verbinding maakt.

Bijwerken naar een nieuwe versie van deze repo: `git pull` in dezelfde map. Je `secrets.yaml` blijft staan.

## A. ESPHome op de opdrachtregel

Installeer Python 3.11 of nieuwer en daarna ESPHome:

```bash
pip install esphome
```

Bouwen en flashen in één keer:

```bash
esphome run aurea-wp.yaml
```

- **Eerste keer:** de ESP32 met een USB-kabel aan de pc. ESPHome vraagt welke poort; kies de COM-poort (Windows) of
  `/dev/ttyUSB0` (Linux).
- **Daarna:** kies het IP-adres of `aurea-wp.local`. Dan gaat het over wifi. Je kunt het ook direct opgeven:

```bash
esphome run aurea-wp.yaml --device 192.168.1.50
```

Alleen bouwen (bijvoorbeeld voor ESPHome Web): `esphome compile aurea-wp.yaml`. Logboek bekijken:
`esphome logs aurea-wp.yaml`.

## B. Het ESPHome-dashboard op je eigen pc

Wil je liever klikken: het dashboard is dezelfde webinterface als de ESPHome Device Builder in Home Assistant.

```bash
esphome dashboard .
```

Open <http://localhost:6052>. Je ziet `aurea-wp` staan; met **INSTALL** kies je *Wirelessly* (OTA), *Plug into this
computer* (USB via de browser) of *Manual download* (een `.bin` voor C).

Met Docker kan het ook, zonder Python te installeren:

```bash
docker run --rm -p 6052:6052 -v "$PWD":/config ghcr.io/esphome/esphome
```

## C. ESPHome Web (alleen flashen, via USB)

[web.esphome.io](https://web.esphome.io) werkt in Chrome of Edge (WebSerial; niet in Firefox of Safari).

1. Bouw de firmware met A (`esphome compile aurea-wp.yaml`) of B (*INSTALL → Manual download → Factory format*).
2. Het bestand voor een eerste installatie is `firmware.factory.bin`; na `esphome compile` staat het in
   `.esphome/build/aurea-wp/.pioenvs/aurea-wp/`.
3. ESP32 met USB aan de pc, op web.esphome.io **CONNECT**, de poort kiezen en **INSTALL** met dat bestand.

Gebruik dit vooral voor de eerste keer of als de ESP32 niet meer via wifi bereikbaar is. Voor gewone updates is A of B
makkelijker.

## Als het misgaat

- **Geen wifi meer:** de ESP32 zet na een minuut een eigen hotspot op (`<friendly_name>_Hotspot`, wachtwoord
  `fallback_password`). Verbind daarmee en vul via de pagina die opent je wifi opnieuw in.
- **OTA weigert:** controleer dat het IP klopt. Lukt het niet, dan via USB (A of C).
- **De AVR in de IC2-voet** werk je hiermee niet bij: dat is een aparte chip met een eigen `.hex`, zie
  [firmware/README.md](../firmware/README.md).
