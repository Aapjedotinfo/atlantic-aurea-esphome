import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID

DEPENDENCIES = ["uart"]

aurea_link_org_ns = cg.esphome_ns.namespace("aurea_link_org")
AureaLinkOrg = aurea_link_org_ns.class_("AureaLinkOrg", cg.PollingComponent, uart.UARTDevice)

CONF_ALLEEN_LUISTEREN = "alleen_luisteren"
CONF_ANTWOORDVERTRAGING = "antwoordvertraging"
CONF_SCHOP_NA = "schop_na"
CONF_LINK_TIMEOUT = "link_timeout"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(AureaLinkOrg),
            # Luisteren zonder te zenden: logt de frames van de IC2 en laat hem verder met rust.
            # Begin hiermee, de frames zijn nog nooit op een echte IC2 bevestigd.
            cv.Optional(CONF_ALLEEN_LUISTEREN, default=False): cv.boolean,
            # Hoe lang na een IC2-frame wij antwoorden (de IC2 negeert bytes in zijn stilteperiode van 25 ms).
            cv.Optional(CONF_ANTWOORDVERTRAGING, default="150ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(milliseconds=50), max=cv.TimePeriod(milliseconds=2000)),
            ),
            # Blijft de IC2 stil, dan sturen wij na deze tijd zelf een frame.
            cv.Optional(CONF_SCHOP_NA, default="30s"): cv.All(
                cv.positive_time_period_seconds,
                cv.Range(min=cv.TimePeriod(seconds=10), max=cv.TimePeriod(seconds=200)),
            ),
            # Zonder IC2-frame binnen deze tijd telt "Brug verbonden" als weg.
            cv.Optional(CONF_LINK_TIMEOUT, default="30s"): cv.All(
                cv.positive_time_period_seconds,
                cv.Range(min=cv.TimePeriod(seconds=10), max=cv.TimePeriod(seconds=240)),
            ),
        }
    )
    .extend(cv.polling_component_schema("2s"))
    .extend(uart.UART_DEVICE_SCHEMA)
)

# De link van de originele firmware: UBRR 103 met U2X geeft 9615 baud bij 8 MHz, 8 databits, 2 stopbits, geen pariteit.
FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "aurea_link_org",
    baud_rate=9615,
    require_tx=True,
    require_rx=True,
    data_bits=8,
    parity="NONE",
    stop_bits=2,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    cg.add(var.set_listen_only(config[CONF_ALLEEN_LUISTEREN]))
    cg.add(var.set_reply_delay(int(config[CONF_ANTWOORDVERTRAGING].total_milliseconds)))
    cg.add(var.set_kick_after(int(config[CONF_SCHOP_NA].total_seconds)))
    cg.add(var.set_link_timeout(int(config[CONF_LINK_TIMEOUT].total_seconds)))
