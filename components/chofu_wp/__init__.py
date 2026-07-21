import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID

DEPENDENCIES = ["uart"]

chofu_wp_ns = cg.esphome_ns.namespace("chofu_wp")
ChofuWP = chofu_wp_ns.class_("ChofuWP", cg.PollingComponent, uart.UARTDevice)

CONF_DT_LOW = "dt_low"
CONF_DT_HIGH = "dt_high"
CONF_MAX_STAND = "max_stand"
CONF_STEP_INTERVAL = "step_interval"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ChofuWP),
            # Regeling (compile-time; zelden nodig - default is bewust rustig)
            cv.Optional(CONF_DT_LOW, default=4.0): cv.float_,
            cv.Optional(CONF_DT_HIGH, default=8.0): cv.float_,
            cv.Optional(CONF_MAX_STAND, default=7): cv.int_range(min=1, max=7),
            cv.Optional(
                CONF_STEP_INTERVAL, default="120s"
            ): cv.positive_time_period_milliseconds,
        }
    )
    .extend(cv.polling_component_schema("5s"))
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_dt_low(config[CONF_DT_LOW]))
    cg.add(var.set_dt_high(config[CONF_DT_HIGH]))
    cg.add(var.set_max_stand(config[CONF_MAX_STAND]))
    cg.add(var.set_step_interval(config[CONF_STEP_INTERVAL]))
