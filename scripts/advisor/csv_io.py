# SPDX-License-Identifier: BSD-3-Clause
"""Reading ``bench/advisor/dataset.csv`` and parsing its cells.

Only ``csv`` is used; the table is small and this keeps the loader free of a
pandas dependency.
"""

from __future__ import annotations

import csv
import math
from pathlib import Path
from typing import Any


def to_float(raw: Any) -> float:
    """Parse a CSV cell into a float, mapping blanks/booleans/garbage sanely."""
    if raw is None:
        return math.nan
    if isinstance(raw, bool):
        return 1.0 if raw else 0.0
    if isinstance(raw, (int, float)):
        value = float(raw)
        return value if math.isfinite(value) else math.nan
    text = raw.strip()
    if not text:
        return math.nan
    lowered = text.lower()
    if lowered in ("true", "yes"):
        return 1.0
    if lowered in ("false", "no"):
        return 0.0
    if lowered in ("nan", "none", "null", "na"):
        return math.nan
    try:
        value = float(text)
    except ValueError:
        return math.nan
    return value if math.isfinite(value) else math.nan


def to_bool(raw: Any) -> bool | None:
    """Tri-state boolean parse: ``None`` when the cell is blank/unknown."""
    if raw is None:
        return None
    if isinstance(raw, bool):
        return raw
    text = str(raw).strip().lower()
    if text in ("true", "1", "yes"):
        return True
    if text in ("false", "0", "no"):
        return False
    return None


def row_key(row: dict[str, str]) -> str:
    """Stable identity for a dataset row, used by the pruning ledger."""
    return "|".join(
        str(row.get(name, "")).strip()
        for name in ("campaign", "cfg_id", "part", "tier")
    )


def read_rows(csv_path: Path) -> list[dict[str, str]]:
    if not csv_path.is_file():
        raise SystemExit(
            f"advisor dataset missing: {csv_path}\n"
            f"build it with: python scripts/build_advisor_dataset.py"
        )
    with csv_path.open("r", newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))
