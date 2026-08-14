#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Build the flat advisor training table from campaign results."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import sys
from collections import Counter
from datetime import datetime, timezone
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

#: Determinism and toolchain probes (ADR-0032). They deliberately re-solve pairs
#: the real campaigns already own, with the SAME cfg_id, so admitting them would
#: hand the builder two contradictory labels for one key and let directory sort
#: order decide. They are evidence, not corpus.
PROBE_CAMPAIGN_GLOBS: tuple[str, ...] = ("xcheck-*", "xstl-*")

#: Explicit precedence when the SAME (cfg_id, part, tier) was solved more than
#: once, newest engine first. The rank is stated here rather than inferred,
#: because every implicit rule available is wrong in some case: directory sort
#: order picks `affected` over `affected2` (wrong) while picking `affected2` over
#: plain `batch-1` (right), and mtime silently depends on file copies.
#:
#: WHY each rank:
#:   1. `advisor-batch-1-affected2-*` -- re-run AFTER the load-rule fix, so both
#:      the CAD and mesh sides select the region the case actually specifies.
#:   2. `advisor-batch-1-affected-*`  -- re-run AFTER traction rescaling but
#:      BEFORE the load-rule fix, so the resultant is mesh-independent yet still
#:      rescaled onto a 0.7-filtered region for `normal_min_dot = -1` cases.
#:   3. `advisor-batch-1-*`           -- PRE-both. Solved under a load that was
#:      scaled by whatever area the candidate mesh happened to select.
#:
#: A stale row frequently SCORES BETTER than its replacement, because it was
#: solved under a wrong load and graded against retired truth: sphere_box_s0_c1
#: cfg-d34d960b reports rel_err 0.7076 (affected), 0.0259 (affected2) and 0.0140
#: (stale batch-1). Training on the stale row teaches that the wrong-load
#: configuration is the most accurate one, and a family-grouped split cannot
#: expose it because both copies land on the same side.
CAMPAIGN_PRIORITY: tuple[str, ...] = (
    "advisor-batch-1-affected2-*",
    "advisor-batch-1-affected-*",
    "advisor-batch-1-*",
)


def campaign_rank(campaign: str) -> int | None:
    """Index in CAMPAIGN_PRIORITY (lower wins), or None when unranked."""
    for rank, pattern in enumerate(CAMPAIGN_PRIORITY):
        if fnmatch(campaign, pattern):
            return rank
    return None


def has_engine_marker(row: dict[str, Any]) -> bool:
    """True when the row was written by a post-traction-rescale testlab.

    `answers.load_area_status` only exists once the load-area gate became
    three-valued, so its presence is a per-row engine-generation marker. It is a
    CROSS-CHECK on CAMPAIGN_PRIORITY, never a second precedence rule: if the list
    ever prefers a row without it over one with it, the list is misconfigured and
    the build fails rather than quietly training on the older row.
    """
    answers = row.get("answers")
    return isinstance(answers, dict) and "load_area_status" in answers


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
TOP_OUTCOMES = [
    "status", "error", "mesh_ms", "solve_ms", "n_dof", "n_elems", "n_nodes",
    "geometry_fill_volume_err", "geometry_volume_err",
]
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
    """Yield ((cfg_id, part, tier), row) for one campaign directory.

    The key deliberately EXCLUDES the campaign name. It used to include it, which
    meant the same (cfg_id, part, tier) solved in two directories never collided
    and both rows reached the dataset -- so re-running a pair on a fixed engine
    added a second, contradictory label instead of replacing the first. Collisions
    are now resolved explicitly by CAMPAIGN_PRIORITY in main().

    Within one directory the warehouse rows are yielded last and still win, which
    is the intended last-seen behaviour for an exact duplicate of the same run.
    """
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
                yield (row.get("cfg_id"), row.get("part"), row.get("tier")), row
    for path in sorted((campaign_dir / "runs").glob("*/*/t*/result.json")):
        row = read_json(path)
        yield (row.get("cfg_id"), row.get("part"), row.get("tier")), row


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

# --- Accuracy re-derivation ------------------------------------------------
# Every campaign row carries a raw ``answers`` block AND an ``accuracy`` block
# that testlab computed against whatever bench/reference/ held AT SOLVE TIME.
# That freezes a truth snapshot into every row, so replacing truth (here:
# self-generated overkill references -> closed-form + CalculiX-on-Gmsh) would
# otherwise mean re-running hours of campaigns to re-score measurements the row
# already contains. We therefore IGNORE the stored ``accuracy`` and re-derive it
# from ``answers`` against the CURRENT references on every build.
#
# The re-derivation below mirrors apps/testlab/main.cpp exactly:
#   load_metrics()   ~line 359  -> load_reference_metrics()
#   evaluate_probe() ~line 1491 -> probe_measured()
#   accuracy loop    ~line 2492 -> rederive_accuracy()
# Any drift between them is a correctness bug, not a style difference.

# probe.kind -> the ProbeAnswers field it scores, and whether evaluate_probe
# divides that field by probe.nominal. Kinds reading a field testlab does not
# write into ``answers`` are unscoreable from a stored row; they are counted and
# reported, never approximated with a neighbouring field.
_PROBE_FIELD: dict[str, str] = {
    "mean_vm": "sigma_face_mean",
    "mean_von_mises": "sigma_face_mean",
    "face_mean_vm": "sigma_face_mean",
    "peak_vm": "sigma_box_max",
    "peak_vm_over_nominal": "sigma_box_max",
    "mean_vm_over_nominal": "sigma_face_mean",
    "scf_mean": "sigma_face_mean",
    "scf": "sigma_face_mean",
    "max_von_mises": "sigma_max",
    "max_vm": "sigma_max",
    "max_vm_over_nominal": "sigma_max",
    "sigma_p99": "sigma_p99",
    "p99_vm": "sigma_p99",
    "strain_energy": "strain_energy",
    "energy": "strain_energy",
    "max_displacement": "tip_deflection",
    "tip_deflection": "tip_deflection",
}
# Kinds normalised by probe.nominal. evaluate_probe throws when nominal == 0, so
# a reference that omits it is malformed and must fail loudly here too.
_PROBE_OVER_NOMINAL = frozenset({
    "peak_vm_over_nominal", "mean_vm_over_nominal", "scf_mean", "scf",
    "max_vm_over_nominal",
})
# Axis-conditional kinds: evaluate_probe picks mean_u_component when the probed
# axis IS the dominant load axis, else the per-axis mean.
_PROBE_AXIS = {"mean_ux_on_face": (0, "mean_ux"), "mean_uz_on_face": (2, "mean_uz")}


def load_reference_metrics(path: Path) -> list[dict[str, Any]]:
    """Mirror of ``load_metrics`` (apps/testlab/main.cpp:359).

    Requires the interfaces.md ``metrics[]`` form; the legacy values-only format
    is rejected there and here. ``tol`` defaults to 0.05, ``nominal`` to 0.0.
    """
    document = read_json(path)
    metrics = document.get("metrics")
    if not isinstance(metrics, list):
        raise ValueError(
            f"reference {path} must use interfaces.md metrics[] (name/value/tol/probe)"
        )
    out: list[dict[str, Any]] = []
    for metric in metrics:
        probe = metric.get("probe", {})
        if not isinstance(probe, dict):
            probe = {}
        out.append({
            "name": str(metric["name"]),
            "value": float(metric["value"]),
            "tol": float(metric.get("tol", 0.05)),
            "probe": {
                "kind": str(probe.get("kind", "")),
                "nominal": float(probe.get("nominal", 0.0)),
            },
        })
    return out


def probe_measured(probe: dict[str, Any], answers: dict[str, Any]) -> tuple[float | None, str]:
    """Mirror of ``evaluate_probe`` (apps/testlab/main.cpp:1491).

    Returns ``(value, "")`` when the probe can be scored from the recorded
    answers, or ``(None, reason)`` when the field it needs was never written to
    the row. An unknown kind, or a zero ``nominal`` on a normalised kind, throws
    exactly as the C++ does: those are malformed references, not missing data.
    """
    kind = probe.get("kind", "")

    def recorded(field: str) -> tuple[float | None, str]:
        value = answers.get(field)
        if not isinstance(value, (int, float)) or isinstance(value, bool):
            return None, f"answers.{field}_absent"
        return float(value), ""

    if kind in _PROBE_AXIS:
        axis, per_axis_field = _PROBE_AXIS[kind]
        dominant = answers.get("dominant_load_axis")
        if not isinstance(dominant, int) or isinstance(dominant, bool):
            return None, "answers.dominant_load_axis_absent"
        return recorded("mean_u_component" if dominant == axis else per_axis_field)

    field = _PROBE_FIELD.get(kind)
    if field is None:
        raise ValueError(f"unknown probe kind '{kind}'")
    value, reason = recorded(field)
    if value is None:
        return None, reason
    if kind in _PROBE_OVER_NOMINAL:
        nominal = probe.get("nominal", 0.0)
        if not abs(nominal) > 0.0:
            raise ValueError(f"{kind} requires probe.nominal != 0")
        return value / nominal, ""
    return value, ""


def rederive_accuracy(
    row: dict[str, Any], metrics: list[dict[str, Any]]
) -> tuple[dict[str, Any] | None, str]:
    """Recompute a row's ``accuracy`` block from raw ``answers`` + current truth.

    Mirror of apps/testlab/main.cpp:2492-2525, including the health gate: when
    the solve failed its residual/reaction/orphan gates, rel_err is still
    recorded but every score is zeroed and ``trusted`` is false, so ranking never
    trusts a singular solve. Returns ``(None, reason)`` when the row cannot be
    re-scored honestly.
    """
    answers = row.get("answers")
    if not isinstance(answers, dict):
        return None, "no_answers"
    health = row.get("health")
    if not isinstance(health, dict) or not isinstance(health.get("ok"), bool):
        # health_ok is what makes a score trusted; without it we cannot
        # reproduce the C++ decision and must not invent one.
        return None, "no_health_ok"
    health_ok = bool(health["ok"])

    detail: list[dict[str, Any]] = []
    for metric in metrics:
        measured, reason = probe_measured(metric["probe"], answers)
        if measured is None:
            return None, reason
        truth = metric["value"]
        rel = abs(measured - truth) / abs(truth) if abs(truth) > 0.0 else abs(measured)
        tol = metric["tol"] if metric["tol"] > 0.0 else 1e-12
        detail.append({
            "metric": metric["name"],
            "value": measured,
            "truth": truth,
            "rel_err": rel,
            "score": (1.0 / (1.0 + rel / tol)) if health_ok else 0.0,
            "trusted": health_ok,
        })

    if not detail:
        # Same empty-metrics shape the C++ emits: no score/trusted/all keys.
        return {"metric": "none", "value": None, "truth": None, "rel_err": None}, ""
    accuracy = dict(detail[0])
    accuracy["all"] = detail
    return accuracy, ""


class ReferenceSet:
    """The CURRENT truth, resolved per part via the case's ``reference`` path.

    Also records provenance (which files, content hash, newest mtime) so a built
    dataset is self-describing about the truth that produced it.
    """

    def __init__(self) -> None:
        self._metrics: dict[str, list[dict[str, Any]]] = {}
        self._used: dict[Path, str] = {}
        self._missing: Counter[str] = Counter()

    def metrics_for(self, part: str, case: dict[str, Any] | None) -> list[dict[str, Any]] | None:
        """Metrics for ``part``, or None when the reference is missing/unreadable."""
        if part in self._metrics:
            return self._metrics[part]
        reference = case.get("reference") if isinstance(case, dict) else None
        if not isinstance(reference, str) or not reference:
            self._missing["no_case_reference"] += 1
            return None
        path = ROOT / reference
        if not path.is_file():
            self._missing["reference_file_missing"] += 1
            return None
        metrics = load_reference_metrics(path)
        self._metrics[part] = metrics
        payload = path.read_bytes()
        self._used[path] = hashlib.sha256(payload).hexdigest()
        return metrics

    @property
    def missing(self) -> Counter[str]:
        return self._missing

    def provenance(self) -> dict[str, Any]:
        if not self._used:
            return {"n_files": 0, "sha256": None, "newest_mtime": None, "roots": []}
        combined = hashlib.sha256()
        for path in sorted(self._used):
            combined.update(path.relative_to(ROOT).as_posix().encode("utf-8"))
            combined.update(self._used[path].encode("ascii"))
        roots = sorted({path.parent.relative_to(ROOT).as_posix() for path in self._used})
        newest = max(path.stat().st_mtime for path in self._used)
        return {
            "n_files": len(self._used),
            "sha256": combined.hexdigest(),
            "newest_mtime": datetime.fromtimestamp(newest, tz=timezone.utc)
            .isoformat(timespec="seconds")
            .replace("+00:00", "Z"),
            "roots": roots,
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
        value = row.get(name, "" if name in STRING_OUTCOMES else np.nan)
        # An explicit null is the engine saying "not measured" (e.g. a resolution
        # refusal fires before any mesh exists, so there is no volume to compare).
        # Map it to NaN deliberately rather than relying on the CSV writer, so the
        # distinction between "unknown" and a real 0.0 survives into the dataset.
        if value is None and name not in STRING_OUTCOMES:
            value = np.nan
        flat[name] = value
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
    #: (cfg_id, part, tier) -> (campaign, row) for the winning row.
    unique: dict[tuple[Any, ...], tuple[str, dict[str, Any]]] = {}
    #: (loser_campaign, winner_campaign) -> rows superseded. A silent supersede is
    #: how a wrong-load row nearly reached a retrain, so the count is reported.
    superseded: Counter[tuple[str, str]] = Counter()
    #: Collisions CAMPAIGN_PRIORITY cannot order. Never silent.
    unordered: list[tuple[tuple[Any, ...], str, str]] = []
    #: Cross-check violations: the list preferred an older-engine row.
    marker_violations: list[str] = []
    skipped_probes: list[str] = []
    for campaign_dir in sorted(path for path in CAMPAIGNS.iterdir() if path.is_dir()):
        if fnmatch(campaign_dir.name, TRUTH_CAMPAIGN_GLOB):
            # promote_truth.py DEFINES each case's reference truth from these
            # rows, so their accuracy_rel_err is ~0 by construction. Training on
            # them would teach the model that the overkill config has zero
            # error: definitionally true, completely non-generalizable.
            skipped_truth.append(campaign_dir.name)
            continue
        if any(fnmatch(campaign_dir.name, pattern) for pattern in PROBE_CAMPAIGN_GLOBS):
            skipped_probes.append(campaign_dir.name)
            continue
        campaign = campaign_dir.name
        for key, row in campaign_records(campaign_dir):
            records_scanned += 1
            previous = unique.get(key)
            if previous is None:
                unique[key] = (campaign, row)
                continue
            held_campaign, held_row = previous
            if held_campaign == campaign:
                # Same directory: warehouse rows are visited last and win an exact
                # duplicate of the same run, which is the original behaviour.
                unique[key] = (campaign, row)
                continue
            held_rank = campaign_rank(held_campaign)
            new_rank = campaign_rank(campaign)
            if held_rank is None or new_rank is None:
                # Cannot order these two. Keep the incumbent deterministically and
                # report it, so a future re-run directory cannot inherit a tie-break
                # by being sorted luckily.
                unordered.append((key, held_campaign, campaign))
                continue
            winner_campaign, winner_row, loser_campaign, loser_row = (
                (campaign, row, held_campaign, held_row)
                if new_rank < held_rank
                else (held_campaign, held_row, campaign, row)
            )
            unique[key] = (winner_campaign, winner_row)
            superseded[(loser_campaign, winner_campaign)] += 1
            if has_engine_marker(loser_row) and not has_engine_marker(winner_row):
                marker_violations.append(
                    f"{key}: CAMPAIGN_PRIORITY chose {winner_campaign} (no "
                    f"answers.load_area_status) over {loser_campaign} (has it)"
                )

    if marker_violations:
        print("error: CAMPAIGN_PRIORITY preferred an older-engine row over a newer one.",
              file=sys.stderr)
        print("       answers.load_area_status exists only in post-rescale rows, so the",
              file=sys.stderr)
        print("       priority list is misconfigured. Refusing to build.", file=sys.stderr)
        for line in marker_violations[:10]:
            print(f"  {line}", file=sys.stderr)
        if len(marker_violations) > 10:
            print(f"  ... and {len(marker_violations) - 10} more", file=sys.stderr)
        return 1

    source_schema_counts: Counter[str] = Counter()
    excluded_legacy_sources: Counter[str] = Counter()
    excluded_legacy_rows = 0
    geometry_refusal_rows = 0

    schema_counts: Counter[str] = Counter()
    failure_signal: Counter[str] = Counter()
    references = ReferenceSet()
    rescored_rows = 0
    unscoreable: Counter[str] = Counter()
    kept: list[dict[str, Any]] = []
    for key in sorted(unique, key=lambda item: tuple("" if value is None else str(value) for value in item)):
        _, part, _ = key
        campaign, row = unique[key]
        source_schema = row.get("schema") if row.get("schema") == "advisor-row-v3" else "legacy"
        source_schema_counts[source_schema] += 1
        if source_schema != "advisor-row-v3":
            excluded_legacy_rows += 1
            excluded_legacy_sources[campaign] += 1
            continue
        # `advisor_training_eligible: false` marks a resolution refusal
        # (`GeometryVolumeLimitError`): no mesh was produced, so the row has no
        # honest accuracy, geometry or cost target. It is excluded outright.
        #
        # Keeping it as a failure-head-only row was tried and MEASURED WORSE, so
        # this exclusion is a result, not an oversight. Adding the 776 refusals
        # (train failure share 8 % -> 28 %) moved every validation number the
        # wrong way on the same held-out fold and the same masked rows:
        # rel_err 0.559 -> 0.711, dof 0.157 -> 0.299, mesh_ms 0.241 -> 0.475 --
        # a 17k-parameter shared trunk spends capacity on them -- and the
        # failure head itself fell from AUC 0.72 to 0.53.
        #
        # The reason it cannot pay is that refusal does not transfer across
        # families. LightGBM trained directly on the failure label scores AUC
        # 0.87 and 0.78 on two held-out families, 0.43 on a third and 0.37 on
        # box_hole -- WORSE than chance, i.e. what other families teach about
        # refusal is actively misleading there, mean 0.61 over four folds. Until
        # a feature carries the refusal boundary itself, these rows are noise
        # with a label attached.
        if row.get("advisor_training_eligible") is False:
            geometry_refusal_rows += 1
            continue

        schema_counts[source_schema] += 1

        # Re-derive accuracy from this row's raw `answers` against the CURRENT
        # reference set, discarding the truth snapshot testlab froze into the row
        # at solve time. Substituting it here (rather than at each use) keeps
        # `trusted()`, the failure signal and the flattened columns consistent by
        # construction. A row we cannot re-score honestly loses its accuracy
        # block entirely -- an empty column is recoverable, a stale one is not.
        case = cases.get(str(part))
        metrics = references.metrics_for(str(part), case)
        if metrics is None:
            accuracy, reason = None, "no_reference"
        else:
            accuracy, reason = rederive_accuracy(row, metrics)
        row = dict(row)
        if accuracy is None:
            row.pop("accuracy", None)
            unscoreable[reason] += 1
        else:
            row["accuracy"] = accuracy
            rescored_rows += 1

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
        kept.append(flatten_row(campaign, row, case))

    fixed = IDENTITY_COLUMNS + FEATURE_COLUMNS + CASE_COLUMNS + ACTION_COLUMNS + TOP_OUTCOMES
    extra = sorted({column for row in kept for column in row if column not in fixed})
    columns = fixed + extra

    print(f"Rows in: {len(unique)} unique ({records_scanned} records scanned, "
          f"{records_scanned - len(unique)} duplicates)")
    print(f"Rows emitted: {len(kept)}")
    if superseded:
        total = sum(superseded.values())
        print(f"Superseded by CAMPAIGN_PRIORITY: {total} row(s) replaced by a "
              "newer-engine re-run of the same (cfg_id, part, tier)")
        for (loser, winner), count in sorted(superseded.items()):
            print(f"  {count:5d}  {loser}  ->  {winner}")
    if unordered:
        parts = sorted({f"{a} vs {b}" for _, a, b in unordered})
        print(f"WARNING: {len(unordered)} collision(s) CAMPAIGN_PRIORITY cannot order; "
              "kept the first directory scanned:")
        for pair in parts[:8]:
            print(f"  {pair}")
        if len(parts) > 8:
            print(f"  ... and {len(parts) - 8} more directory pairs")
        print("  Add the directory to CAMPAIGN_PRIORITY so the winner is chosen, "
              "not inherited from sort order.")
    if excluded_legacy_rows:
        excluded = ", ".join(
            f"{CAMPAIGNS.relative_to(ROOT).as_posix()}/{campaign}/results.jsonl={count}"
            for campaign, count in sorted(excluded_legacy_sources.items())
        )
        print(f"Legacy rows excluded from training dataset: {excluded_legacy_rows} ({excluded})")
    if geometry_refusal_rows:
        print(f"Resolution-refusal rows excluded (measured worse when kept): "
              f"{geometry_refusal_rows}")

    # Truth provenance: a dataset must be self-describing about the references
    # that scored it, because accuracy is now re-derived at build time and the
    # reference set is expected to change (self-generated -> third-party).
    truth = references.provenance()
    print(f"Accuracy re-derived from raw answers: {rescored_rows} rows "
          f"(stored per-row accuracy snapshots ignored)")
    if unscoreable:
        detail = ", ".join(f"{reason}={count}" for reason, count in sorted(unscoreable.items()))
        print(f"Rows left unscored (no accuracy columns emitted): {sum(unscoreable.values())} "
              f"({detail})")
    if references.missing:
        detail = ", ".join(f"{reason}={count}" for reason, count in sorted(references.missing.items()))
        print(f"Parts with no usable reference: {detail}")
    print(f"Truth set: {truth['n_files']} reference files under "
          + (", ".join(truth["roots"]) if truth["roots"] else "(none)")
          + f" sha256={truth['sha256'][:16] if truth['sha256'] else 'none'} "
          + f"newest={truth['newest_mtime'] or 'n/a'}")
    print(f"Truth campaigns skipped ({TRUTH_CAMPAIGN_GLOB}): "
          + (", ".join(skipped_truth) if skipped_truth else "none")
          + "  [their rel_err is ~0 by construction; promote_truth.py defines truth from them]")
    if skipped_probes:
        print("Determinism/toolchain probe campaigns skipped: " + ", ".join(skipped_probes)
              + "  [ADR-0032 evidence; they re-solve corpus pairs under a different build]")
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
        # Accuracy columns are re-derived at build time, so the dataset records
        # WHICH truth produced them. Re-running this build against a different
        # reference set is the supported way to re-score, and this block is how a
        # consumer tells two such datasets apart.
        "truth": {
            "rederived_from": "row answers[] via probe kinds mirrored from apps/testlab/main.cpp",
            "rows_rescored": rescored_rows,
            "rows_unscored": sum(unscoreable.values()),
            "unscored_reasons": dict(sorted(unscoreable.items())),
            **references.provenance(),
        },
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
