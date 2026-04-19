import esphome.codegen as cg
from esphome.components import i2c

CODEOWNERS = ["@kenavn"]
DEPENDENCIES = ["i2c"]
MULTI_CONF = True

m32jm_ns = cg.esphome_ns.namespace("m32jm")
M32JM = m32jm_ns.class_("M32JM", cg.PollingComponent, i2c.I2CDevice)
