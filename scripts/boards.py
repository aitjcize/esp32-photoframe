import json
from pathlib import Path

_BOARDS_JSON = Path(__file__).resolve().parent.parent / "boards" / "boards.json"

with open(_BOARDS_JSON, encoding="utf-8") as _f:
    BOARDS = json.load(_f)

BOARDS_BY_ID = {b["id"]: b for b in BOARDS}

SUPPORTED_BOARDS = {b["id"]: f'{b["label"]} ({b["display"]})' for b in BOARDS}

BOARD_DIMENSIONS = {b["id"]: tuple(b["resolution"]) for b in BOARDS}

# Display color model per board: "gc16" (16-level grayscale), "spectra6" (6-color,
# the default). Used to pick the dithering palette for splash generation, etc.
BOARD_DISPLAY_TYPE = {b["id"]: b.get("display_type", "spectra6") for b in BOARDS}

# ESP-IDF target chip per board. All boards are ESP32-S3 except the M5Paper,
# which is a plain ESP32 (ESP32-D0WDQ6-V3). Used to pass -DIDF_TARGET to
# idf.py and to pick the esptool --chip / bootloader offset.
BOARD_TARGET = {b["id"]: b.get("target", "esp32s3") for b in BOARDS}

# Flash offset of the 2nd-stage bootloader, which differs per chip family.
_BOOTLOADER_OFFSET = {"esp32": "0x1000"}

# ESP Web Tools chipFamily string per IDF target.
_CHIP_FAMILY = {"esp32": "ESP32", "esp32s3": "ESP32-S3"}


def board_chip_family(board):
    """ESP Web Tools chipFamily name for a board's target chip."""
    return _CHIP_FAMILY.get(BOARD_TARGET.get(board, "esp32s3"), "ESP32-S3")


def board_flash_args(board):
    """(esptool chip name, bootloader flash offset) for a board."""
    target = BOARD_TARGET.get(board, "esp32s3")
    return target, _BOOTLOADER_OFFSET.get(target, "0x0")
