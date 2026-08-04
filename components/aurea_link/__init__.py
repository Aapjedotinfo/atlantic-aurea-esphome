import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID

DEPENDENCIES = ["uart"]

aurea_link_ns = cg.esphome_ns.namespace("aurea_link")
AureaLink = aurea_link_ns.class_("AureaLink", cg.PollingComponent, uart.UARTDevice)

CONF_BRAKE_SETPOINT = "brake_setpoint"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(AureaLink),
            # Waarde die de rem richting de ketel oplegt. 10 °C is wat de
            # originele Atlantic-firmware ook schreef om de ketel koest te
            # houden (0x0a00 bij adres 0x04be in T1.1).
            cv.Optional(CONF_BRAKE_SETPOINT, default=10.0): cv.float_range(
                min=0.0, max=90.0
            ),
        }
    )
    .extend(cv.polling_component_schema("2s"))
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_brake_setpoint(config[CONF_BRAKE_SETPOINT]))
