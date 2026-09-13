import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import CONF_KEY

from . import KeyBridgeComponent

DEPENDENCIES = ["KeyBridge"]

CONF_KEYBRIDGE_ID = "KeyBridge_id"

PAGE_KEYBOARD = 0x07
PAGE_CONSUMER = 0x0C
PAGE_GENERIC_DESKTOP = 0x01

KB = PAGE_KEYBOARD
CC = PAGE_CONSUMER
GD = PAGE_GENERIC_DESKTOP


def _u(*pairs):
    return list(pairs)


# HID usages. A name may map to more than one (keyboard + consumer media keys).
KEYS = {
    # Letters (usage is the same with or without Shift)
    "a": _u((KB, 0x04)),
    "b": _u((KB, 0x05)),
    "c": _u((KB, 0x06)),
    "d": _u((KB, 0x07)),
    "e": _u((KB, 0x08)),
    "f": _u((KB, 0x09)),
    "g": _u((KB, 0x0A)),
    "h": _u((KB, 0x0B)),
    "i": _u((KB, 0x0C)),
    "j": _u((KB, 0x0D)),
    "k": _u((KB, 0x0E)),
    "l": _u((KB, 0x0F)),
    "m": _u((KB, 0x10)),
    "n": _u((KB, 0x11)),
    "o": _u((KB, 0x12)),
    "p": _u((KB, 0x13)),
    "q": _u((KB, 0x14)),
    "r": _u((KB, 0x15)),
    "s": _u((KB, 0x16)),
    "t": _u((KB, 0x17)),
    "u": _u((KB, 0x18)),
    "v": _u((KB, 0x19)),
    "w": _u((KB, 0x1A)),
    "x": _u((KB, 0x1B)),
    "y": _u((KB, 0x1C)),
    "z": _u((KB, 0x1D)),
    # Number row (not keypad)
    "1": _u((KB, 0x1E)),
    "2": _u((KB, 0x1F)),
    "3": _u((KB, 0x20)),
    "4": _u((KB, 0x21)),
    "5": _u((KB, 0x22)),
    "6": _u((KB, 0x23)),
    "7": _u((KB, 0x24)),
    "8": _u((KB, 0x25)),
    "9": _u((KB, 0x26)),
    "0": _u((KB, 0x27)),
    # Editing / whitespace
    "enter": _u((KB, 0x28)),
    "return": _u((KB, 0x28)),
    "escape": _u((KB, 0x29)),
    "esc": _u((KB, 0x29)),
    "backspace": _u((KB, 0x2A)),
    "tab": _u((KB, 0x2B)),
    "space": _u((KB, 0x2C)),
    "minus": _u((KB, 0x2D)),
    "equal": _u((KB, 0x2E)),
    "left_bracket": _u((KB, 0x2F)),
    "right_bracket": _u((KB, 0x30)),
    "backslash": _u((KB, 0x31)),
    "semicolon": _u((KB, 0x33)),
    "apostrophe": _u((KB, 0x34)),
    "grave": _u((KB, 0x35)),
    "comma": _u((KB, 0x36)),
    "dot": _u((KB, 0x37)),
    "slash": _u((KB, 0x38)),
    "caps_lock": _u((KB, 0x39)),
    # Function
    "f1": _u((KB, 0x3A)),
    "f2": _u((KB, 0x3B)),
    "f3": _u((KB, 0x3C)),
    "f4": _u((KB, 0x3D)),
    "f5": _u((KB, 0x3E)),
    "f6": _u((KB, 0x3F)),
    "f7": _u((KB, 0x40)),
    "f8": _u((KB, 0x41)),
    "f9": _u((KB, 0x42)),
    "f10": _u((KB, 0x43)),
    "f11": _u((KB, 0x44)),
    "f12": _u((KB, 0x45)),
    "print_screen": _u((KB, 0x46)),
    "scroll_lock": _u((KB, 0x47)),
    "pause": _u((KB, 0x48)),
    # Navigation
    "insert": _u((KB, 0x49)),
    "home": _u((KB, 0x4A)),
    "page_up": _u((KB, 0x4B)),
    "delete": _u((KB, 0x4C)),
    "end": _u((KB, 0x4D)),
    "page_down": _u((KB, 0x4E)),
    "right": _u((KB, 0x4F)),
    "left": _u((KB, 0x50)),
    "down": _u((KB, 0x51)),
    "up": _u((KB, 0x52)),
    # Keypad
    "num_lock": _u((KB, 0x53)),
    "kp_divide": _u((KB, 0x54)),
    "kp_multiply": _u((KB, 0x55)),
    "kp_minus": _u((KB, 0x56)),
    "kp_plus": _u((KB, 0x57)),
    "kp_enter": _u((KB, 0x58)),
    "kp_1": _u((KB, 0x59)),
    "kp_2": _u((KB, 0x5A)),
    "kp_3": _u((KB, 0x5B)),
    "kp_4": _u((KB, 0x5C)),
    "kp_5": _u((KB, 0x5D)),
    "kp_6": _u((KB, 0x5E)),
    "kp_7": _u((KB, 0x5F)),
    "kp_8": _u((KB, 0x60)),
    "kp_9": _u((KB, 0x61)),
    "kp_0": _u((KB, 0x62)),
    "kp_dot": _u((KB, 0x63)),
    "application": _u((KB, 0x65)),
    "menu": _u((KB, 0x65), (CC, 0x40)),
    # Modifiers
    "left_ctrl": _u((KB, 0xE0)),
    "left_shift": _u((KB, 0xE1)),
    "left_alt": _u((KB, 0xE2)),
    "left_gui": _u((KB, 0xE3)),
    "right_ctrl": _u((KB, 0xE4)),
    "right_shift": _u((KB, 0xE5)),
    "right_alt": _u((KB, 0xE6)),
    "right_gui": _u((KB, 0xE7)),
    # Media — consumer page plus keyboard-page twins
    "mute": _u((CC, 0xE2), (KB, 0x7F)),
    "volume_up": _u((CC, 0xE9), (KB, 0x80)),
    "volume_down": _u((CC, 0xEA), (KB, 0x81)),
    "play_pause": _u((CC, 0xCD)),
    "play": _u((CC, 0xB0)),
    "pause_media": _u((CC, 0xB1)),
    "record": _u((CC, 0xB2)),
    "fast_forward": _u((CC, 0xB3)),
    "rewind": _u((CC, 0xB4)),
    "next_track": _u((CC, 0xB5)),
    "prev_track": _u((CC, 0xB6)),
    "stop": _u((CC, 0xB7)),
    "eject": _u((CC, 0xB8)),
    "media_select": _u((CC, 0x183)),
    "mail": _u((CC, 0x18A)),
    "calculator": _u((CC, 0x192)),
    "browser": _u((CC, 0x196)),
    "ac_home": _u((CC, 0x223)),
    "ac_back": _u((CC, 0x224)),
    "ac_forward": _u((CC, 0x225)),
    # System
    "power": _u((CC, 0x30), (KB, 0x66), (GD, 0x81)),
    "sleep": _u((GD, 0x82)),
    "wake": _u((GD, 0x83)),
}


def validate_key(value):
    if isinstance(value, int) and not isinstance(value, bool):
        if 1 <= value <= 0xFFFF:
            return _u((PAGE_KEYBOARD, value))
        raise cv.Invalid("numeric key must be a keyboard usage 1-65535")
    name = cv.string(value).strip().lower().replace(" ", "_").replace("-", "_")
    if name not in KEYS:
        raise cv.Invalid(
            f"Unknown key '{value}'. Use a letter, a named key (enter, volume_up, ...), "
            "or a numeric keyboard usage."
        )
    return KEYS[name]


CONFIG_SCHEMA = binary_sensor.binary_sensor_schema().extend(
    {
        cv.GenerateID(CONF_KEYBRIDGE_ID): cv.use_id(KeyBridgeComponent),
        cv.Required(CONF_KEY): validate_key,
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_KEYBRIDGE_ID])
    var = await binary_sensor.new_binary_sensor(config)
    for page, usage in config[CONF_KEY]:
        cg.add(parent.set_key_sensor(page, usage, var))
