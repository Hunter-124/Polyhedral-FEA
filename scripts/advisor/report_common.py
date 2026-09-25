# SPDX-License-Identifier: BSD-3-Clause
"""Shared readers and output helpers for the ``report.py`` figures."""
from __future__ import annotations

import csv
import json
import math
import re
from pathlib import Path
from typing import Any

import numpy as np

import figstyle as fs

#: committed PNGs stay small; the palette pass in figstyle.finish handles the
#: mesh renders, which are the only figures here that get near the ceiling.
MAX_PNG_BYTES = 400 * 1024

#: only these rows carry the full C2 feature/action vector (see dataset.py)
CORPUS_SCHEMA = "advisor-row-v3"
#: statuses that produced a usable solve
OK_STATUS = "ok"
#: parts are named ``<family>_s<shape>_c<case>``; the family is the stem
FAMILY_RE = re.compile(r"_s\d+_c\d+$")


def tint(color: str, amount: float = 0.78) -> str:
    """Lighten a palette colour towards the page, for large filled areas."""
    from matplotlib.colors import to_hex, to_rgb

    rgb = np.array(to_rgb(color))
    page = np.array(to_rgb(fs.theme().panel))
    return to_hex(rgb + (page - rgb) * amount)


def load_json(path: Path) -> dict[str, Any] | None:
    if not path.is_file():
        return None
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def to_float(value: Any) -> float:
    """CSV cell -> float, with blanks and junk mapped to NaN."""
    try:
        result = float(value)
    except (TypeError, ValueError):
        return math.nan
    return result if math.isfinite(result) else math.nan


def family_of(part: str) -> str:
    return FAMILY_RE.sub("", part)


def read_rows(dataset_csv: Path) -> list[dict[str, str]]:
    if not dataset_csv.is_file():
        return []
    with dataset_csv.open("r", encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def corpus_rows(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    return [row for row in rows if row.get("schema") == CORPUS_SCHEMA]


def save(fig: Any, out_dir: Path, name: str) -> Path:
    return fs.finish(fig, out_dir / name, max_bytes=MAX_PNG_BYTES)


def fmt(value: float, digits: int = 4) -> str:
    if not math.isfinite(value):
        return "n/a"
    return f"{value:.{digits}g}"
