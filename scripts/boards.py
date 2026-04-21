import json
from pathlib import Path

_BOARDS_JSON = Path(__file__).resolve().parent.parent / "boards" / "boards.json"

with open(_BOARDS_JSON, encoding="utf-8") as _f:
    BOARDS = json.load(_f)

BOARDS_BY_ID = {b["id"]: b for b in BOARDS}

SUPPORTED_BOARDS = {b["id"]: f'{b["label"]} ({b["display"]})' for b in BOARDS}

BOARD_DIMENSIONS = {b["id"]: tuple(b["resolution"]) for b in BOARDS}
