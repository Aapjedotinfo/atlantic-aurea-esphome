import re

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import text_sensor, uart
from esphome.const import CONF_ID, CONF_TRIGGER_ID

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["text_sensor"]

aurea_link2_ns = cg.esphome_ns.namespace("aurea_link2")
AureaLink2 = aurea_link2_ns.class_("AureaLink2", cg.PollingComponent, uart.UARTDevice)
VraagTrigger = aurea_link2_ns.class_(
    "VraagTrigger",
    automation.Trigger.template(cg.uint8, cg.uint8, cg.float_, cg.uint32),
)
DoorgevenAction = aurea_link2_ns.class_(
    "DoorgevenAction", automation.Action, cg.Parented.template(AureaLink2)
)
BlokkerenAction = aurea_link2_ns.class_(
    "BlokkerenAction", automation.Action, cg.Parented.template(AureaLink2)
)
VervangenAction = aurea_link2_ns.class_(
    "VervangenAction", automation.Action, cg.Parented.template(AureaLink2)
)

CONF_BRAKE_SETPOINT = "brake_setpoint"
CONF_ONDERSCHEPPEN = "onderscheppen"
CONF_STANDAARDREGELS = "standaardregels"
CONF_ON_VRAAG = "on_vraag"
CONF_DATA_ID = "data_id"
CONF_RICHTING = "richting"
CONF_MODUS = "modus"
CONF_PARAMETER = "parameter"
CONF_NAAM = "naam"
CONF_ACTIEF = "actief"
CONF_CONDITIE = "conditie"
CONF_MASKER = "masker"
CONF_WIS = "wis"
CONF_ZET = "zet"
CONF_FORCEER = "forceer"
CONF_ECHO = "echo"
CONF_DEADLINE = "deadline"
CONF_STANDAARD = "standaard"
CONF_TABELVERSIE = "tabelversie"
CONF_DEADLINE_ANTWOORD = "deadline_antwoord"
CONF_WACHTTIJD_KETEL = "wachttijd_ketel"
CONF_ESP_WEG_TIJD = "esp_weg_tijd"
CONF_NOODBRUGTIJD = "noodbrugtijd"
CONF_REM_HOUDTIJD = "rem_houdtijd"
CONF_GELDIGHEID = "geldigheid"
CONF_KETELTYPE = "keteltype"
CONF_MINIMALE_VERSIE = "minimale_versie"
CONF_AVR_VERSIE = "avr_versie"
CONF_AVR_MELDING = "avr_melding"
CONF_FRAME = "frame"
CONF_WAARDE = "waarde"

RICHTING = {"T>K": 0, "K>T": 1}
MODUS = {"pass": 0, "vast": 1, "vraag": 2, "blokkeer": 3}
CONDITIE = {
    "altijd": 0,
    "rem": 1,
    "substitutie": 2,
    "override": 3,
    "ch_off": 4,
    "rem_of_ch_off": 5,
    "max_mod": 6,
}
# Bijzondere waarden voor VAST (PROTOCOL.md 5.2) en de woorden voor de andere modi.
SPECIAAL = {
    "rem": 0xFFFF,
    "aanvoer": 0xFFFE,
    "retour": 0xFFFD,
    "override": 0xFFFC,
    "max_mod": 0xFFFB,
}
STANDAARDACTIE = {"doorgeven": 0, "blokkeren": 1}
BLOKKEERTYPE = {"onbekend": 7, "ongeldig": 6}
KETELTYPE = {"dip": 0, "opentherm": 1, "aan_uit": 2}

# De ingebouwde standaardtabel van de AVR (PROTOCOL.md 5.3), met DHW-enable aan in regel 1.
# (naam, id, richting, modusbyte, p0, p1). Met `standaardregels: true` is dit het begin van de tabel;
# een eigen regel met dezelfde (id, richting) vervangt de standaardregel.
STANDAARD_TABEL = [
    ("std_ch_vlag", 0, 0, 0x35, 0x01, 0x02),
    ("std_rem_setpoint", 1, 0, 0x05, 0xFF, 0xFF),
    ("std_kamer_setpoint", 16, 0, 0x45, 0x08, 0x00),
    ("std_max_mod", 14, 0, 0x19, 0xFF, 0xFB),
    ("std_echo_id1", 1, 1, 0x85, 0x00, 0x00),
    ("std_echo_id16", 16, 1, 0x85, 0x00, 0x00),
    ("std_aanvoer", 25, 1, 0x09, 0xFF, 0xFE),
    ("std_retour", 28, 1, 0x09, 0xFF, 0xFD),
    ("std_override_id9", 9, 1, 0x4D, 0xFF, 0xFC),
    ("std_override_id100", 100, 1, 0x4D, 0x00, 0x00),
]


_richting = cv.All(cv.one_of(*RICHTING, upper=True), lambda v: RICHTING[v])


def _tijd(min_ms, max_ms):
    return cv.All(
        cv.positive_time_period_milliseconds,
        cv.Range(
            min=cv.TimePeriod(milliseconds=min_ms),
            max=cv.TimePeriod(milliseconds=max_ms),
        ),
    )


def _regel_basis(conf):
    """Controleer een regel en reken hem om naar (modusbyte, p0, p1)."""
    modus = conf[CONF_MODUS]
    richting = conf[CONF_RICHTING]
    par = conf.get(CONF_PARAMETER)
    vlaggen = (
        conf.get(CONF_MASKER, False),
        conf.get(CONF_FORCEER, False),
        conf.get(CONF_ECHO, False),
    )
    if modus != "vast" and any(vlaggen):
        raise cv.Invalid("masker, forceer en echo kunnen alleen bij modus: vast")
    if vlaggen[0] and richting != 0:
        raise cv.Invalid("masker kan alleen bij richting T>K")
    if vlaggen[2] and richting != 1:
        raise cv.Invalid("echo kan alleen bij richting K>T")
    if vlaggen[0] and (CONF_WIS not in conf and CONF_ZET not in conf):
        raise cv.Invalid("masker vraagt om wis: en/of zet: (bits in de hoge byte)")
    if not vlaggen[0] and (CONF_WIS in conf or CONF_ZET in conf):
        raise cv.Invalid("wis: en zet: horen bij masker: true")

    p0 = p1 = 0
    if modus == "pass":
        if par is not None:
            raise cv.Invalid("modus pass heeft geen parameter")
    elif modus == "vast":
        if vlaggen[0]:
            if par is not None:
                raise cv.Invalid("bij masker horen wis: en zet:, geen parameter")
            p0, p1 = conf.get(CONF_WIS, 0), conf.get(CONF_ZET, 0)
        else:
            if par is None:
                par = 0 if vlaggen[2] else None
            if par is None:
                raise cv.Invalid(
                    "modus vast vraagt om een parameter (16-bit waarde of rem, aanvoer, retour, override, max_mod)"
                )
            if isinstance(par, str):
                if par not in SPECIAAL:
                    raise cv.Invalid(f"parameter '{par}' hoort niet bij modus vast")
                par = SPECIAAL[par]
            p0, p1 = par >> 8, par & 0xFF
    elif modus == "vraag":
        dl = conf.get(CONF_DEADLINE)
        if dl is not None:
            ms = int(dl.total_milliseconds)
            if ms > 600:
                raise cv.Invalid("deadline per regel: hoogstens 600 ms")
            p0 = (ms + 3) // 4  # eenheden van 4 ms; 0 = standaard-deadline
        act = conf.get(CONF_STANDAARD, par if isinstance(par, str) else "doorgeven")
        if act not in STANDAARDACTIE:
            raise cv.Invalid("standaard (parameter) bij vraag: doorgeven of blokkeren")
        p1 = STANDAARDACTIE[act]
    else:  # blokkeer
        t = par if par is not None else "onbekend"
        if isinstance(t, str):
            if t not in BLOKKEERTYPE:
                raise cv.Invalid("parameter bij blokkeer: onbekend of ongeldig")
            t = BLOKKEERTYPE[t]
        if t not in (0, 6, 7):
            raise cv.Invalid("parameter bij blokkeer: 0, 6, 7, onbekend of ongeldig")
        p1 = t

    byte = (
        MODUS[modus]
        | (CONDITIE[conf[CONF_CONDITIE]] << 2)
        | (0x20 if vlaggen[0] else 0)
        | (0x40 if vlaggen[1] else 0)
        | (0x80 if vlaggen[2] else 0)
    )
    return byte, p0, p1


PARAMETER = cv.Any(
    cv.int_range(min=0, max=0xFFFF),
    cv.one_of(*SPECIAAL, *STANDAARDACTIE, *BLOKKEERTYPE, lower=True),
)

REGEL_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required("id"): cv.int_range(min=0, max=255),
            cv.Required(CONF_RICHTING): _richting,
            cv.Optional(CONF_MODUS, default="pass"): cv.one_of(*MODUS, lower=True),
            cv.Optional(CONF_PARAMETER): PARAMETER,
            cv.Optional(CONF_NAAM): cv.string,
            cv.Optional(CONF_ACTIEF, default=True): cv.boolean,
            cv.Optional(CONF_CONDITIE, default="altijd"): cv.one_of(*CONDITIE, lower=True),
            cv.Optional(CONF_MASKER, default=False): cv.boolean,
            cv.Optional(CONF_WIS): cv.hex_uint8_t,
            cv.Optional(CONF_ZET): cv.hex_uint8_t,
            cv.Optional(CONF_FORCEER, default=False): cv.boolean,
            cv.Optional(CONF_ECHO, default=False): cv.boolean,
            cv.Optional(CONF_DEADLINE): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_STANDAARD): cv.one_of(*STANDAARDACTIE, lower=True),
        }
    ),
    lambda c: (_regel_basis(c), c)[1],  # alleen controleren
)


def _geldigheid(t):
    if t.total_seconds != 0 and not 10 <= t.total_seconds <= 255:
        raise cv.Invalid("geldigheid: 0 of 10..255 s")
    return t


def _versie(value):
    value = cv.string(value)
    m = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", value.strip())
    if not m:
        raise cv.Invalid("verwacht een versie als 2.0.0")
    return tuple(int(x) for x in m.groups())


def _tabel_controle(config):
    """Bouw de definitieve regellijst (standaardregels + eigen) en controleer de grenzen."""
    regels = []  # (naam, id, richting, modus, p0, p1, actief)
    if config[CONF_STANDAARDREGELS]:
        regels += [(n, i, r, m, a, b, True) for (n, i, r, m, a, b) in STANDAARD_TABEL]
    for idx, c in enumerate(config.get(CONF_ONDERSCHEPPEN, [])):
        modus, p0, p1 = _regel_basis(c)
        naam = c.get(CONF_NAAM, f"regel{idx + 1}")
        nieuw = (naam, c["id"], c[CONF_RICHTING], modus, p0, p1, c[CONF_ACTIEF])
        # Een eigen regel vervangt een standaardregel met dezelfde (id, richting).
        regels = [x for x in regels if not (x[1] == nieuw[1] and x[2] == nieuw[2] and x[0].startswith("std_"))]
        regels.append(nieuw)

    actief = [x for x in regels if x[6]]
    sleutels = [(x[1], x[2]) for x in actief]
    if len(sleutels) != len(set(sleutels)):
        raise cv.Invalid("onderscheppen: dubbele regel voor dezelfde (id, richting)")
    if len(actief) > 16:
        raise cv.Invalid(f"onderscheppen: {len(actief)} actieve regels, de AVR kent er hoogstens 16")
    namen = [x[0] for x in regels]
    if len(namen) != len(set(namen)):
        raise cv.Invalid("onderscheppen: namen moeten uniek zijn")

    nood = config[CONF_NOODBRUGTIJD].total_seconds
    weg = config[CONF_ESP_WEG_TIJD].total_seconds
    if nood < weg:
        raise cv.Invalid("noodbrugtijd moet minstens esp_weg_tijd zijn")
    config = dict(config)
    config["_regels"] = regels
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(AureaLink2),
            # Waarde die de rem richting de ketel oplegt; 10 graden is wat de originele Atlantic-firmware schreef.
            cv.Optional(CONF_BRAKE_SETPOINT, default=10.0): cv.float_range(min=0.0, max=90.0),
            cv.Optional(CONF_KETELTYPE, default="dip"): cv.one_of(*KETELTYPE, lower=True),
            # 0 = de AVR gebruikt zijn eigen esp_weg_tijd; anders 10..255 s.
            cv.Optional(CONF_GELDIGHEID, default="0s"): cv.All(cv.positive_time_period_seconds, _geldigheid),
            cv.Optional(CONF_TABELVERSIE, default=1): cv.int_range(min=0, max=255),
            cv.Optional(CONF_DEADLINE_ANTWOORD, default="250ms"): _tijd(30, 600),
            cv.Optional(CONF_WACHTTIJD_KETEL, default="400ms"): _tijd(100, 600),
            cv.Optional(CONF_ESP_WEG_TIJD, default="60s"): _tijd(10_000, 600_000),
            cv.Optional(CONF_NOODBRUGTIJD, default="240s"): _tijd(60_000, 1_800_000),
            cv.Optional(CONF_REM_HOUDTIJD, default="300s"): _tijd(60_000, 900_000),
            cv.Optional(CONF_MINIMALE_VERSIE, default="2.0.0"): _versie,
            cv.Optional(CONF_STANDAARDREGELS, default=False): cv.boolean,
            cv.Optional(CONF_ONDERSCHEPPEN): cv.ensure_list(REGEL_SCHEMA),
            cv.Optional(CONF_AVR_VERSIE): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_AVR_MELDING): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_ON_VRAAG): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(VraagTrigger),
                    cv.Optional(CONF_DATA_ID): cv.int_range(min=0, max=255),
                    cv.Optional(CONF_RICHTING): _richting,
                }
            ),
        }
    )
    .extend(cv.polling_component_schema("2s"))
    .extend(uart.UART_DEVICE_SCHEMA),
    _tabel_controle,
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "aurea_link2",
    baud_rate=38400,
    require_tx=True,
    require_rx=True,
    data_bits=8,
    parity="NONE",
    stop_bits=1,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    cg.add(var.set_brake_setpoint(config[CONF_BRAKE_SETPOINT]))
    cg.add(var.set_boiler_type(KETELTYPE[config[CONF_KETELTYPE]]))
    cg.add(var.set_valid_time(int(config[CONF_GELDIGHEID].total_seconds)))
    cg.add(var.set_tabel_versie(config[CONF_TABELVERSIE]))
    cg.add(
        var.set_tijden(
            int(config[CONF_DEADLINE_ANTWOORD].total_milliseconds),
            int(config[CONF_WACHTTIJD_KETEL].total_milliseconds),
            int(config[CONF_ESP_WEG_TIJD].total_seconds),
            int(config[CONF_NOODBRUGTIJD].total_seconds),
            int(config[CONF_REM_HOUDTIJD].total_seconds),
        )
    )
    hoofd, neven, patch = config[CONF_MINIMALE_VERSIE]
    cg.add(var.set_minimale_versie(hoofd, neven, patch))

    for naam, rid, richting, modus, p0, p1, actief in config["_regels"]:
        cg.add(var.add_regel(rid, richting, modus, p0, p1, naam, actief))

    if CONF_AVR_VERSIE in config:
        sens = await text_sensor.new_text_sensor(config[CONF_AVR_VERSIE])
        cg.add(var.set_avr_versie_sensor(sens))
    if CONF_AVR_MELDING in config:
        sens = await text_sensor.new_text_sensor(config[CONF_AVR_MELDING])
        cg.add(var.set_avr_melding_sensor(sens))

    for conf in config.get(CONF_ON_VRAAG, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        if CONF_DATA_ID in conf:
            cg.add(trigger.set_data_id(conf[CONF_DATA_ID]))
        if CONF_RICHTING in conf:
            cg.add(trigger.set_richting(conf[CONF_RICHTING]))
        cg.add(var.add_vraag_trigger(trigger))
        await automation.build_automation(
            trigger,
            [
                (cg.uint8, "data_id"),
                (cg.uint8, "richting"),
                (cg.float_, "waarde"),
                (cg.uint32, "frame"),
            ],
            conf,
        )


# ── Acties voor on_vraag ──
# doorgeven: het frame ongewijzigd verder.  blokkeren: de AVR antwoordt lokaal UNKNOWN-DATAID.
# vervangen: frame: (4 bytes als lijst of een getal) of waarde: (f8.8, zelfde type en Data-ID).
ACTIE_SCHEMA = automation.maybe_simple_id({cv.GenerateID(): cv.use_id(AureaLink2)})


def _frame(value):
    if isinstance(value, list):
        if len(value) != 4:
            raise cv.Invalid("frame: precies 4 bytes")
        b = [cv.hex_uint8_t(x) for x in value]
        return (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]
    return cv.hex_uint32_t(value)


VERVANGEN_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(AureaLink2),
            cv.Optional(CONF_FRAME): cv.templatable(_frame),
            cv.Optional(CONF_WAARDE): cv.templatable(cv.float_),
        }
    ),
    cv.has_exactly_one_key(CONF_FRAME, CONF_WAARDE),
)

automation.register_parented_action(
    "aurea_link2.doorgeven", DoorgevenAction, ACTIE_SCHEMA, synchronous=True
)
automation.register_parented_action(
    "aurea_link2.blokkeren", BlokkerenAction, ACTIE_SCHEMA, synchronous=True
)


@automation.register_action(
    "aurea_link2.vervangen", VervangenAction, VERVANGEN_SCHEMA, synchronous=True
)
async def vervangen_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    if CONF_FRAME in config:
        cg.add(var.set_frame(await cg.templatable(config[CONF_FRAME], args, cg.uint32)))
    else:
        cg.add(var.set_waarde(await cg.templatable(config[CONF_WAARDE], args, cg.float_)))
    return var
