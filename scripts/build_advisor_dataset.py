#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Build the flat advisor training table from campaign results."""

from __future__ import annotations

import argparse
import csv
import json
from collections import Counter
from fnmatch import fnmatch
from pathlib import Path
from typing import Any, Iterable

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
CAMPAIGNS = ROOT / "bench" / "campaigns"
CASE_DIR = ROOT / "tests" / "fixtures" / "parts"
# Procedural advisor corpus (scripts/gen_primitive_corpus.py) keeps its case JSON next
# to the generated STEP, so both directories must be scanned.
CORPUS_CASE_DIR = ROOT / "bench" / "geometries" / "corpus" / "primitives"
CASE_DIRS = (CASE_DIR, CORPUS_CASE_DIR)
OUTPUT_DIR = ROOT / "bench" / "advisor"

# promote_truth.py DEFINES each case's reference truth from the rows of the
# advisor-truth-* campaigns, so their own accuracy_rel_err is ~0 by
# construction. Training on them would teach the model that the overkill config
# has zero error -- definitionally true and completely non-generalizable.
TRUTH_CAMPAIGN_GLOB = "advisor-truth-*"

FEATURE_COLUMNS = [
    "bbox_dx", "bbox_dy", "bbox_dz", "diag", "volume", "surface_area",
    "sa_over_v23", "n_faces", "n_sharp_edges", "sharp_edge_len_total",
    "curved_frac", "kappa_max_h", "kappa_mean_h", "thin_min_over_diag",
    "thin_p10_over_diag", "min_feature_h", "n_fix_faces", "n_load_faces",
    "fix_area_frac", "load_area_frac", "load_dir_x", "load_dir_y", "load_dir_z",
    "fix_load_dist_over_diag", "load_axis_alignment", "poisson",
]
ACTION_COLUMNS = [
    "h", "h_rel", "mesher", "element_tendency", "skin_layers", "feature_refine",
    "bc_grading", "adapt_passes", "eta_target", "p_elevate", "adapt_leb_waves", "order",
]
IDENTITY_COLUMNS = ["schema", "campaign", "cfg_id", "part", "tier"]
CASE_COLUMNS = [
    "case_poisson", "case_n_fix_regions", "case_n_load_regions", "case_load_dir_x",
    "case_load_dir_y", "case_load_dir_z", "case_traction_magnitude",
]
# ``error`` is the row's top-level failure string; it is the first signal
# dataset.py::_failure_flag looks at, so it has to reach the CSV.
TOP_OUTCOMES = ["status", "error", "mesh_ms", "solve_ms", "n_dof", "n_elems", "n_nodes"]
STRING_OUTCOMES = frozenset({"status", "error"})


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dry-run", action="store_true", help="read and summarize without writing")
    return parser.parse_args()


def read_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return value


def campaign_records(campaign_dir: Path) -> Iterable[tuple[tuple[Any, ...], dict[str, Any]]]:
    results = campaign_dir / "results.jsonl"
    if results.exists():
        with results.open("r", encoding="utf-8") as stream:
            for line_number, text in enumerate(stream, 1):
                if not text.strip():
                    continue
                try:
                    row = json.loads(text)
                except json.JSONDecodeError as exc:
                    raise ValueError(f"{results}:{line_number}: {exc}") from exc
                key = (campaign_dir.name, row.get("cfg_id"), row.get("part"), row.get("tier"))
                yield key, row
    for path in sorted((campaign_dir / "runs").glob("*/*/t*/result.json")):
        row = read_json(path)
        key = (campaign_dir.name, row.get("cfg_id"), row.get("part"), row.get("tier"))
        yield key, row


def load_cases() -> dict[str, dict[str, Any]]:
    cases: dict[str, dict[str, Any]] = {}
    for case_dir in CASE_DIRS:
        for path in sorted(case_dir.glob("*.case.json")):
            case = read_json(path)
            part = case.get("part")
            if isinstance(part, str):
                cases[part] = case
    return cases


def case_counts() -> str:
    return ", ".join(
        f"{case_dir.relative_to(ROOT).as_posix()}={len(list(case_dir.glob('*.case.json')))}"
        for case_dir in CASE_DIRS
    )


def case_context(case: dict[str, Any] | None) -> dict[str, Any]:
    if case is None:
        return {name: np.nan for name in CASE_COLUMNS}
    material = case.get("material", {})
    loads = case.get("loads", [])
    traction = np.zeros(3, dtype=np.float64)
    for load in loads:
        values = load.get("traction", [0.0, 0.0, 0.0])
        if isinstance(values, list) and len(values) == 3:
            traction += np.asarray(values, dtype=np.float64)
    magnitude = float(np.linalg.norm(traction))
    direction = traction / magnitude if magnitude > 0.0 else np.zeros(3, dtype=np.float64)
    return {
        "case_poisson": material.get("nu", material.get("poissons_ratio", np.nan)),
        "case_n_fix_regions": len(case.get("bcs", [])),
        "case_n_load_regions": len(loads),
        "case_load_dir_x": float(direction[0]),
        "case_load_dir_y": float(direction[1]),
        "case_load_dir_z": float(direction[2]),
        "case_traction_magnitude": magnitude,
    }


def flatten_scalars(prefix: str, value: Any, output: dict[str, Any]) -> None:
    if isinstance(value, dict):
        for key in sorted(value):
            flatten_scalars(f"{prefix}_{key}", value[key], output)
    elif isinstance(value, (str, int, float, bool)) or value is None:
        output[prefix] = value
    # Arrays (notably accuracy.all) are deliberately omitted from a one-row table.


def trusted(row: dict[str, Any]) -> bool | None:
    accuracy = row.get("accuracy")
    if not isinstance(accuracy, dict):
        return None
    value = accuracy.get("trusted")
    return value if isinstance(value, bool) else None


def flatten_row(campaign: str, row: dict[str, Any], case: dict[str, Any] | None) -> dict[str, Any]:
    schema = row.get("schema") if row.get("schema") == "advisor-row-v3" else "legacy"
    flat: dict[str, Any] = {
        "schema": schema,
        "campaign": campaign,
        "cfg_id": row.get("cfg_id", ""),
        "part": row.get("part", ""),
        "tier": row.get("tier", np.nan),
    }
    features = row.get("features", {}) if schema == "advisor-row-v3" else {}
    for name in FEATURE_COLUMNS:
        flat[name] = features.get(name, np.nan)
    action = row.get("action", {}) if schema == "advisor-row-v3" else {}
    legacy = row.get("config", {}) if schema == "legacy" else {}
    for name in ACTION_COLUMNS:
        if schema == "legacy" and name in {"mesher", "feature_refine", "order"}:
            flat[name] = legacy.get(name, np.nan)
        else:
            flat[name] = action.get(name, np.nan)
    flat.update(case_context(case))
    for name in TOP_OUTCOMES:
        # `status`/`error` are strings: an absent one means "no error", not
        # "unknown". A NaN here would reach the CSV as the literal "nan" and
        # dataset.py::_failure_flag would score every row as a failure.
        flat[name] = row.get(name, "" if name in STRING_OUTCOMES else np.nan)
    for group in ("accuracy", "answers", "health", "quality", "geo_fidelity", "scorecard"):
        value = row.get(group)
        if isinstance(value, dict):
            flatten_scalars(group, value, flat)
    return flat


def role_of(column: str) -> str:
    if column in ACTION_COLUMNS:
        return "action"
    if column in FEATURE_COLUMNS or column in CASE_COLUMNS:
        return "context"
    if column in IDENTITY_COLUMNS:
        return "context"
    return "outcome"

def description_of(column: str) -> str:
    identity = {
        "schema": "Source campaign-row schema version.",
        "campaign": "Campaign directory containing the run.",
        "cfg_id": "Stable campaign configuration identifier.",
        "part": "Fixture part identifier joined to its case JSON.",
        "tier": "Successive-halving tier index.",
    }
    if column in identity:
        return identity[column]
    if column in FEATURE_COLUMNS:
        return f"Advisor context feature '{column}'; geometric values are bbox-normalized."
    if column in CASE_COLUMNS:
        return f"Boundary-condition context '{column}' derived from the part case JSON."
    if column in ACTION_COLUMNS:
        return f"Swept mesh-advisor action '{column}'."
    if column == "status":
        return "Campaign run completion or failure status."
    if column == "error":
        return ("Top-level failure message; empty when the run produced no "
                "error. Primary signal for the advisor feasibility head.")
    if column.startswith("accuracy_"):
        return f"Flattened accuracy outcome field '{column.removeprefix('accuracy_')}'."
    if column.startswith("answers_"):
        return f"Flattened measured answer '{column.removeprefix('answers_')}'."
    if column.startswith("health_"):
        return f"Flattened solve-health outcome '{column.removeprefix('health_')}'."
    if column.startswith("quality_"):
        return f"Flattened mesh-quality outcome '{column.removeprefix('quality_')}'."
    if column.startswith("scorecard_"):
        return f"Flattened campaign scorecard outcome '{column.removeprefix('scorecard_')}'."
    return f"Campaign outcome '{column}'."


def dtype_of(values: list[Any]) -> str:
    present = [value for value in values if value is not None and not (isinstance(value, float) and np.isnan(value))]
    if not present:
        return "number"
    if all(isinstance(value, bool) for value in present):
        return "boolean"
    if all(isinstance(value, (int, float, np.integer, np.floating)) and not isinstance(value, bool) for value in present):
        return "number"
    return "string"


def main() -> int:
    args = parse_args()
    cases = load_cases()
    print(f"Cases loaded: {len(cases)} ({case_counts()})")
    records_scanned = 0
    skipped_truth: list[str] = []
    unique: dict[tuple[Any, ...], dict[str, Any]] = {}
    for campaign_dir in sorted(path for path in CAMPAIGNS.iterdir() if path.is_dir()):
        if fnmatch(campaign_dir.name, TRUTH_CAMPAIGN_GLOB):
            # promote_truth.py DEFINES each case's reference truth from these
            # rows, so their accuracy_rel_err is ~0 by construction. Training on
            # them would teach the model that the overkill config has zero
            # error: definitionally true, completely non-generalizable.
            skipped_truth.append(campaign_dir.name)
            continue
        for key, row in campaign_records(campaign_dir):
            records_scanned += 1
            unique[key] = row  # warehouse rows are visited last and win exact duplicates

    source_schema_counts: Counter[str] = Counter()
    excluded_legacy_sources: Counter[str] = Counter()
    excluded_legacy_rows = 0
    schema_counts: Counter[str] = Counter()
    failure_signal: Counter[str] = Counter()
    kept: list[dict[str, Any]] = []
    for key in sorted(unique, key=lambda item: tuple("" if value is None else str(value) for value in item)):
        campaign, _, part, _ = key
        row = unique[key]
        source_schema = row.get("schema") if row.get("schema") == "advisor-row-v3" else "legacy"
        source_schema_counts[source_schema] += 1
        if source_schema != "advisor-row-v3":
            excluded_legacy_rows += 1
            excluded_legacy_sources[campaign] += 1
            continue
        schema_counts[source_schema] += 1
        # Unhealthy and untrusted rows are KEPT: they are the only supervision
        # the feasibility head has, and dataset.py masks them out of every
        # regression head via _failure_flag. Dropping them here made two of the
        # three failure signals dead by construction.
        health = row.get("health")
        unhealthy = isinstance(health, dict) and health.get("ok") is False
        untrusted = trusted(row) is False
        if unhealthy:
            failure_signal["health_not_ok"] += 1
        if untrusted:
            failure_signal["accuracy_untrusted"] += 1
        if unhealthy or untrusted:
            failure_signal["rows"] += 1
        kept.append(flatten_row(campaign, row, cases.get(str(part))))

    fixed = IDENTITY_COLUMNS + FEATURE_COLUMNS + CASE_COLUMNS + ACTION_COLUMNS + TOP_OUTCOMES
    extra = sorted({column for row in kept for column in row if column not in fixed})
    columns = fixed + extra

    print(f"Rows in: {len(unique)} unique ({records_scanned} records scanned, "
          f"{records_scanned - len(unique)} duplicates)")
    print(f"Rows emitted: {len(kept)}")
    if excluded_legacy_rows:
        excluded = ", ".join(
            f"{CAMPAIGNS.relative_to(ROOT).as_posix()}/{campaign}/results.jsonl={count}"
            for campaign, count in sorted(excluded_legacy_sources.items())
        )
        print(f"Legacy rows excluded from training dataset: {excluded_legacy_rows} ({excluded})")
    print(f"Truth campaigns skipped ({TRUTH_CAMPAIGN_GLOB}): "
          + (", ".join(skipped_truth) if skipped_truth else "none")
          + "  [their rel_err is ~0 by construction; promote_truth.py defines truth from them]")
    print(f"Rows retained for the failure head: {failure_signal['rows']} "
          f"(health_not_ok={failure_signal['health_not_ok']}, "
          f"accuracy_untrusted={failure_signal['accuracy_untrusted']})")
    print("Source schemas: " + ", ".join(
        f"{name}={source_schema_counts[name]}" for name in sorted(source_schema_counts)
    ))
    print("Schemas emitted: " + ", ".join(f"{name}={schema_counts[name]}" for name in sorted(schema_counts)))

    if args.dry_run:
        print("Dry run: no files written")
        return 0

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    dataset_path = OUTPUT_DIR / "dataset.csv"
    with dataset_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(kept)

    schema = {
        "schema": "advisor-dataset-schema-v1",
        "source_row_schemas": sorted(schema_counts),
        "columns": [
            {
                "name": column,
                "role": role_of(column),
                "dtype": dtype_of([row.get(column, np.nan) for row in kept]),
                "description": description_of(column),
            }
            for column in columns
        ],
    }
    schema_path = OUTPUT_DIR / "dataset_schema.json"
    with schema_path.open("w", encoding="utf-8") as stream:
        json.dump(schema, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(f"Wrote: {dataset_path.relative_to(ROOT)}")
    print(f"Wrote: {schema_path.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
