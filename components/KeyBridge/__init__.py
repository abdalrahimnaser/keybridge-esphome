import esphome.codegen as cg
from esphome.components.esp32 import (
    VARIANT_ESP32S2,
    VARIANT_ESP32S3,
    add_idf_sdkconfig_option,
    only_on_variant,
)
import esphome.config_validation as cv
from esphome.const import CONF_ID

DEPENDENCIES = ["esp32"]

keybridge_ns = cg.esphome_ns.namespace("keybridge")
KeyBridgeComponent = keybridge_ns.class_("KeyBridgeComponent", cg.Component)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(KeyBridgeComponent),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    only_on_variant(supported=[VARIANT_ESP32S2, VARIANT_ESP32S3]),
)


async def to_code(config):
    add_idf_sdkconfig_option("CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE", 1024)
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
