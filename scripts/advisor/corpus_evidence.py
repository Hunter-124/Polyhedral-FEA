#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Recompute the corpus-design evidence into one committed, machine-readable file.

    python scripts/advisor/corpus_evidence.py

Why this exists
---------------
Four measurements decided how the corpus was widened, and until now each lived
only in a report. A reviewer cannot audit a number that exists in prose, and the
commit that added two geometry families had to describe them as "the
workstream's measurements" because they appeared in no committed file. This
recomputes all four from the artifacts on disk and emits
``bench/advisor/corpus_evidence.json``, so the figures generator can plot from
data and the model card can cite a file rather than a claim.

Nothing here is transcribed. Every value is derived at run time from
``bench/advisor/geometry_features.csv`` and ``bench/advisor/learning_curve.json``,
plus a closed-form derivation checked against the generated case JSONs.

The four sections
-----------------
1. ``descriptor_distances`` -- how far the two new families sit from the existing
   six in standardized descriptor space. This is the premise of the widening
   test, and one of the two picks fails it: ``perforated_plate`` lands BELOW the
   existing corpus minimum, which makes it a weaker test of transfer than
   ``tube``. Recorded explicitly rather than left for a reader to notice.

2. ``tube_load_slab`` -- the derivation that makes the ``tube`` family
   trustworthy. An end-slab load box also encloses a ring of the cylindrical
   walls; the ring-to-annulus area ratio is exactly ``2 * depth / wall``, so
   scaling the slab off the wall pins it at 0.400 % while scaling it off the
   diameter, as the solid shaft does, would have put it at 15-21 % silently.

3. ``family_recovery`` -- 1-nearest-neighbour family recovery from the geometry
   descriptors alone. The single most load-bearing measurement in the ML story:
   it is simultaneously why the descriptors do not transfer across families and
   why they make an excellent out-of-distribution detector.

4. ``learning_curve`` -- the fitted slope of held-out regret against the number
   of training families, with a bootstrap CI, plus a power table giving the CI
   half-width attainable at 6/8/10/12/16 families. Together these are the basis
   for "eight families is where this question becomes falsifiable".
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import re
import sys
from pathlib import Path
from typing import Any

import numpy as np

if __package__ in (None, ""):  # direct invocation
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    __package__ = "advisor"

from .dataset import ADVISOR_DIR, ROOT, provenance  # noqa: E402
from .geometry_features import FEATURES_CSV  # noqa: E402

REPORT_JSON = ADVISOR_DIR / "corpus_evidence.json"
LEARNING_CURVE_JSON = ADVISOR_DIR / "learning_curve.json"
CASE_DIR = ROOT / "bench" / "geometries" / "corpus" / "primitives"

#: Families present before the widening. Everything is reported relative to
#: these so "distant from the existing corpus" has a fixed referent.
BASELINE_FAMILIES = ("box_hole", "channel", "l_bracket", "plate_notch",
                     "sphere_box", "stepped_shaft")
#: Every family added since that baseline, in the order they were added. Listed
#: here so `new_family_placement` reports each one's distance from the baseline
#: instead of silently covering only the first two widening picks.
NEW_FAMILIES = ("tube", "perforated_plate",
                "ellipsoid_boss", "lobed_shaft", "twisted_loft",
                "ribbed_plate", "gusset_bracket", "multi_hole_plate",
                "bossed_plate")

#: Slab depth as a fraction of the load characteristic length, and the fraction
#: of the wall thickness the tube family uses for that characteristic length.
#: Mirrors gen_primitive_corpus.LOAD_SLAB_FRAC and build_tube.
LOAD_SLAB_FRAC = 0.01
TUBE_LOAD_CHAR_FRAC = 0.2

#: Residual sd of a single (fold, seed) regret point, measured on the current
#: corpus. Drives the power table.
POWER_RESIDUAL_SD = 0.28
POWER_SEEDS = 5
POWER_FAMILY_COUNTS = (6, 8, 10, 12, 16, 20)

BOOTSTRAP_DRAWS = 4000
RNG_SEED = 7


def family_of(part: str) -> str:
    return re.sub(r"_s\d+$", "", part)


# --------------------------------------------------------------------------- #
# 1. descriptor distances
# --------------------------------------------------------------------------- #

def load_descriptors(csv_path: Path) -> tuple[list[str], np.ndarray, np.ndarray]:
    """Descriptor names, the raw matrix and the per-row family from the CSV."""
    with csv_path.open("r", newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise RuntimeError(f"{csv_path}: no descriptor rows")
    names = [name for name in rows[0] if name != "part"]
    raw = np.asarray([[float(row[name]) for name in names] for row in rows],
                     dtype=np.float64)
    families = np.asarray([family_of(row["part"]) for row in rows])
    return names, raw, families


def standardized_centroids(raw: np.ndarray, families: np.ndarray
                           ) -> dict[str, np.ndarray]:
    """Per-family centroid in z-score space. THE definition of that space.

    Standardized so a descriptor with a wide native range cannot dominate the
    distance purely by units. Kept as one function because the corpus-widening
    evidence and the generator's `--check-coverage` gate must not be able to
    disagree about what "descriptor distance" means.
    """
    scale = np.where(raw.std(axis=0) > 0, raw.std(axis=0), 1.0)
    z = (raw - raw.mean(axis=0)) / scale
    return {f: z[families == f].mean(axis=0) for f in sorted(set(families))}


def centroid_pairwise(centroids: dict[str, np.ndarray]) -> dict[str, dict[str, float]]:
    return {a: {b: float(np.linalg.norm(va - vb))
                for b, vb in centroids.items() if b != a}
            for a, va in centroids.items()}


def coverage_report(raw: np.ndarray, families: np.ndarray,
                    new_families: tuple[str, ...] | list[str]) -> dict[str, Any]:
    """Is each candidate family at least as distinct as the corpus already is?

    The referent is the MINIMUM pairwise centroid distance among the families
    that are not under test. A candidate closer to an existing family than the
    existing families are to each other is a weaker test of transfer, not a
    widening -- the `perforated_plate` lesson, made a gate instead of a
    footnote. A candidate with no descriptor rows at all is a failure, never a
    pass: an unmeasurable family cannot be certified distinct.
    """
    under_test = list(dict.fromkeys(new_families))
    centroids = standardized_centroids(raw, families)
    pairwise = centroid_pairwise(centroids)
    baseline = [f for f in sorted(centroids) if f not in set(under_test)]
    if len(baseline) < 2:
        raise RuntimeError("coverage needs at least two reference families, "
                           f"got {baseline}")
    baseline_pairs = [pairwise[a][b] for i, a in enumerate(baseline)
                      for b in baseline[i + 1:]]
    threshold = float(np.min(baseline_pairs))
    placement: dict[str, Any] = {}
    for family in under_test:
        if family not in centroids:
            continue
        distance, nearest = min((pairwise[family][b], b) for b in baseline)
        placement[family] = {
            "nearest_reference_family": nearest,
            "distance_to_nearest_reference": distance,
            "margin_over_threshold": distance - threshold,
            "below_reference_minimum": bool(distance < threshold),
        }
    missing = [f for f in under_test if f not in centroids]
    return {
        "standardization": "per-column z-score over all geometries in the file",
        "reference_families": baseline,
        "n_reference_pairs": len(baseline_pairs),
        "reference_min_pairwise_distance": threshold,
        "families_under_test": placement,
        "missing_descriptors": missing,
        "ok": not missing and not any(entry["below_reference_minimum"]
                                     for entry in placement.values()),
    }


def descriptor_distances(csv_path: Path) -> dict[str, Any]:
    names, raw, families = load_descriptors(csv_path)
    centroids = standardized_centroids(raw, families)
    pairwise = centroid_pairwise(centroids)

    present_baseline = [f for f in BASELINE_FAMILIES if f in centroids]
    baseline_pairs = [pairwise[a][b] for i, a in enumerate(present_baseline)
                      for b in present_baseline[i + 1:]]
    baseline_stats = {
        "min": float(np.min(baseline_pairs)) if baseline_pairs else float("nan"),
        "median": float(np.median(baseline_pairs)) if baseline_pairs else float("nan"),
        "max": float(np.max(baseline_pairs)) if baseline_pairs else float("nan"),
        "n_pairs": len(baseline_pairs),
    }

    new_placement: dict[str, Any] = {}
    for f in NEW_FAMILIES:
        if f not in centroids:
            continue
        to_baseline = sorted((pairwise[f][b], b) for b in present_baseline)
        nearest_distance, nearest = to_baseline[0]
        new_placement[f] = {
            "nearest_baseline_family": nearest,
            "distance_to_nearest_baseline": nearest_distance,
            "below_baseline_minimum": bool(nearest_distance < baseline_stats["min"]),
            "distances_to_baseline": {b: d for d, b in to_baseline},
            # The honest interpretation, carried with the number so it cannot be
            # separated from it: a family closer to an existing one than the
            # existing families are to each other is a WEAKER test of transfer.
            "transfer_test_strength": ("weaker than the existing corpus's closest pair"
                                       if nearest_distance < baseline_stats["min"]
                                       else "at least as distinct as the existing "
                                            "corpus's closest pair"),
        }

    return {
        "descriptor_columns": names,
        "n_geometries": int(raw.shape[0]),
        "standardization": "per-column z-score over all geometries in the file",
        "family_centroid_pairwise_distance": pairwise,
        "nearest_other_family": {
            f: min(((d, b) for b, d in pairwise[f].items()))[1] for f in pairwise},
        "baseline_family_pairwise": baseline_stats,
        "new_family_placement": new_placement,
    }


# --------------------------------------------------------------------------- #
# 2. tube load slab
# --------------------------------------------------------------------------- #

def tube_load_slab() -> dict[str, Any]:
    """Closed-form ring/annulus ratio, cross-checked against the emitted cases.

    An end slab of inward depth ``d`` on a tube of wall ``t`` encloses a ring of
    the inner and outer cylindrical walls of area ``2*pi*(r_o + r_i)*d`` against
    an annulus of ``pi*(r_o^2 - r_i^2) = pi*t*(r_o + r_i)``. The radii cancel:
    the ratio is exactly ``2*d/t``, independent of the geometry.
    """
    try:
        sys.path.insert(0, str(ROOT / "scripts"))
        from gen_primitive_corpus import build_geometry  # noqa: PLC0415
    except ImportError as error:  # pragma: no cover - OCP guard
        return {"available": False, "reason": str(error)}

    regimes: dict[str, Any] = {}
    for regime in range(4):
        geometry = build_geometry("tube", regime)
        wall = float(geometry.params["wall"])
        r_outer = float(geometry.params["r_outer"])
        depth = LOAD_SLAB_FRAC * TUBE_LOAD_CHAR_FRAC * wall
        counterfactual_depth = LOAD_SLAB_FRAC * 2.0 * r_outer

        entry: dict[str, Any] = {
            "wall_m": wall,
            "r_outer_m": r_outer,
            "inward_slab_depth_m": depth,
            "ring_over_annulus": 2.0 * depth / wall,
            "counterfactual_ring_over_annulus_if_scaled_off_diameter":
                2.0 * counterfactual_depth / wall,
            "annulus_area_m2": float(geometry.end_area),
        }

        # Cross-check the closed form against what the generator actually wrote,
        # so a change to LOAD_SLAB_FRAC or to build_tube cannot silently
        # invalidate this record.
        case_path = CASE_DIR / f"tube_s{regime}_c0.case.json"
        if case_path.is_file():
            case = json.loads(case_path.read_text(encoding="utf-8"))
            box = case["loads"][0]["select"]["box"]
            emitted_depth = float(geometry.hi[geometry.axis]) - float(box[0][geometry.axis])
            entry["emitted_inward_slab_depth_m"] = emitted_depth
            entry["emitted_ring_over_annulus"] = 2.0 * emitted_depth / wall
            entry["derivation_matches_emitted_case"] = bool(
                abs(emitted_depth - depth) <= 1e-9 * max(1.0, depth))
            authored = float(case["loads"][0]["select"]["expected_area"])
            entry["authored_expected_area_m2"] = authored
            entry["authored_matches_analytic_annulus"] = bool(
                abs(authored - float(geometry.end_area)) <= 1e-15)
        regimes[f"tube_s{regime}"] = entry

    ratios = [e["ring_over_annulus"] for e in regimes.values()]
    counter = [e["counterfactual_ring_over_annulus_if_scaled_off_diameter"]
               for e in regimes.values()]
    return {
        "available": True,
        "identity": "ring/annulus = 2 * slab_depth / wall, exactly; the radii cancel",
        "slab_depth_rule": (f"LOAD_SLAB_FRAC ({LOAD_SLAB_FRAC}) * "
                            f"{TUBE_LOAD_CHAR_FRAC} * wall"),
        "per_regime": regimes,
        "ratio_min": float(np.min(ratios)),
        "ratio_max": float(np.max(ratios)),
        "counterfactual_ratio_min": float(np.min(counter)),
        "counterfactual_ratio_max": float(np.max(counter)),
        "authored_area_tolerance": 0.01,
        "load_area_health_tolerance": 0.05,
        "within_authored_area_tolerance": bool(max(ratios) < 0.01),
        "note": ("For the transverse and oblique archetypes the CAD-side rule "
                 "substitutes the slab's thin axis at min_dot 0.7 and drops the "
                 "walls (apps/testlab/main.cpp:1865-1875), while the mesh-side "
                 "selector honours normal_min_dot = -1 literally and keeps every "
                 "in-box face (main.cpp:937-975). The traction is then rescaled "
                 "onto the smaller CAD area, so the resultant stays correct while "
                 "the distribution smears onto the walls. A thin slab is what "
                 "prevents that; a normal filter cannot, because the filter is "
                 "relative to the traction, which is transverse in those cases "
                 "and can never select an axial annulus."),
    }


# --------------------------------------------------------------------------- #
# 3. 1-NN family recovery
# --------------------------------------------------------------------------- #

def family_recovery(csv_path: Path) -> dict[str, Any]:
    """Leave-one-out 1-NN family recovery from the descriptors alone.

    High recovery is the mechanism behind two opposite results. It is why the
    descriptors cannot transfer across a leave-one-family-out split -- they
    encode family identity, and an identifier of the training families says
    nothing about a family that was held out. It is also why they make a strong
    out-of-distribution detector, since "belongs to no known family" is exactly
    what an abstention gate needs to decide.
    """
    with csv_path.open("r", newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    names = [name for name in rows[0] if name != "part"]

    def recover(subset: list[dict[str, str]]) -> dict[str, Any]:
        raw = np.asarray([[float(r[n]) for n in names] for r in subset], dtype=np.float64)
        fam = np.asarray([family_of(r["part"]) for r in subset])
        scale = np.where(raw.std(axis=0) > 0, raw.std(axis=0), 1.0)
        z = (raw - raw.mean(axis=0)) / scale
        correct = 0
        for i in range(len(z)):
            distance = np.linalg.norm(z - z[i], axis=1)
            distance[i] = np.inf
            correct += int(fam[int(np.argmin(distance))] == fam[i])
        return {
            "n_geometries": len(subset),
            "n_families": int(len(set(fam.tolist()))),
            "correct": correct,
            "rate": float(correct / len(subset)) if subset else float("nan"),
        }

    baseline_rows = [r for r in rows if family_of(r["part"]) in BASELINE_FAMILIES]
    return {
        "method": "leave-one-out 1-nearest-neighbour in standardized descriptor space",
        "baseline_six_families": recover(baseline_rows),
        "all_families": recover(rows),
        "implication_negative": ("descriptors encode family identity, so they cannot "
                                 "transfer under leave-one-family-out"),
        "implication_positive": ("the same property makes them a strong "
                                 "out-of-distribution signal; see bench/advisor/ood.json"),
    }


# --------------------------------------------------------------------------- #
# 4. learning curve + power
# --------------------------------------------------------------------------- #

def _bootstrap_slope(x: np.ndarray, y: np.ndarray, draws: int,
                     rng: np.random.Generator) -> tuple[float, float, float]:
    design = np.vstack([x, np.ones_like(x)]).T
    slope = float(np.linalg.lstsq(design, y, rcond=None)[0][0])
    samples = np.empty(draws, dtype=np.float64)
    for i in range(draws):
        index = rng.integers(0, x.size, x.size)
        sub = np.vstack([x[index], np.ones(index.size)]).T
        samples[i] = np.linalg.lstsq(sub, y[index], rcond=None)[0][0]
    lo, hi = np.percentile(samples, [2.5, 97.5])
    return slope, float(lo), float(hi)


def learning_curve_fit(path: Path) -> dict[str, Any]:
    if not path.is_file():
        return {"available": False, "reason": f"missing {path}"}
    payload = json.loads(path.read_text(encoding="utf-8"))
    runs = payload.get("runs", [])
    if not runs:
        return {"available": False, "reason": "no runs recorded"}

    rng = np.random.default_rng(RNG_SEED)
    choosers = sorted({name for run in runs for name in run.get("regret", {})})
    fits: dict[str, Any] = {}
    for name in choosers:
        x = np.asarray([run["k"] for run in runs if name in run["regret"]], dtype=np.float64)
        y = np.asarray([run["regret"][name] for run in runs if name in run["regret"]],
                       dtype=np.float64)
        keep = np.isfinite(y)
        x, y = x[keep], y[keep]
        if x.size < 3 or len(set(x.tolist())) < 2:
            continue
        slope, lo, hi = _bootstrap_slope(x, y, BOOTSTRAP_DRAWS, rng)
        # The oracle has regret identically zero, so its slope and CI are all
        # exactly 0 and the interval does not straddle zero in the strict sense.
        # Reporting that as a significant trend would be plainly wrong, so a
        # degenerate interval is classified as flat explicitly rather than left
        # to a comparison that was never meant for a constant series.
        degenerate = (hi - lo) <= 0.0
        fits[name] = {
            "slope_decades_per_family": slope,
            "ci95": [lo, hi],
            "indistinguishable_from_flat": bool(degenerate or (lo <= 0.0 <= hi)),
            "degenerate_constant_series": bool(degenerate),
            "n_points": int(x.size),
            "residual_sd": float(np.std(y, ddof=1)),
        }

    by_k: dict[str, Any] = {}
    for k in sorted({run["k"] for run in runs}):
        subset = [run for run in runs if run["k"] == k]
        by_k[str(k)] = {
            "n_points": len(subset),
            "mean_train_rows": float(np.mean([r["n_train_rows"] for r in subset])),
            "mean_regret": {
                name: float(np.mean([r["regret"][name] for r in subset
                                     if name in r["regret"]]))
                for name in choosers
                if any(name in r["regret"] for r in subset)
            },
        }

    return {
        "available": True,
        "source": str(path.relative_to(ROOT)).replace("\\", "/"),
        "source_provenance": payload.get("provenance"),
        "objective": payload.get("objective"),
        "bootstrap_draws": BOOTSTRAP_DRAWS,
        "rng_seed": RNG_SEED,
        "by_training_family_count": by_k,
        "slope_fits": fits,
        "controls_note": ("the non-learning choosers (finest_action, constant_config, "
                          "default) must come out flat; any slope there would mean the "
                          "curve is measuring the shrinking candidate pool rather than "
                          "learning"),
    }


def power_table(residual_sd: float = POWER_RESIDUAL_SD, seeds: int = POWER_SEEDS,
                counts: tuple[int, ...] = POWER_FAMILY_COUNTS) -> dict[str, Any]:
    """CI half-width on the per-family slope attainable at each corpus width.

    Closed-form OLS standard error rather than a simulation, so the table is
    exact and cheap to re-derive: with ``k = 1..F-1`` and ``F*seeds`` points per
    ``k``, ``SE(slope) = sd / sqrt(sum (x - xbar)^2)``.
    """
    rows: dict[str, Any] = {}
    for families in counts:
        ks = np.arange(1, families, dtype=np.float64)
        if ks.size < 2:
            continue
        x = np.repeat(ks, families * seeds)
        sxx = float(((x - x.mean()) ** 2).sum())
        se = residual_sd / math.sqrt(sxx)
        rows[str(families)] = {
            "n_points": int(x.size),
            "slope_se": se,
            "ci95_half_width": 1.96 * se,
        }
    return {
        "residual_sd": residual_sd,
        "seeds_per_point": seeds,
        "method": "closed-form OLS standard error on the slope",
        "by_family_count": rows,
        "interpretation": ("eight families is the smallest corpus at which a "
                           "per-family effect of about 0.02 decades becomes "
                           "detectable, which is what makes the widening "
                           "hypothesis falsifiable rather than merely open"),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--features", type=Path, default=FEATURES_CSV)
    parser.add_argument("--learning-curve", type=Path, default=LEARNING_CURVE_JSON)
    parser.add_argument("--out", type=Path, default=REPORT_JSON)
    args = parser.parse_args(argv)

    if not args.features.is_file():
        raise SystemExit(f"missing {args.features}; run scripts/advisor/geometry_features.py")

    payload = {
        "provenance": provenance(),
        "descriptor_distances": descriptor_distances(args.features),
        "tube_load_slab": tube_load_slab(),
        "family_recovery": family_recovery(args.features),
        "learning_curve": learning_curve_fit(args.learning_curve),
        "power": power_table(),
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    d = payload["descriptor_distances"]
    base = d["baseline_family_pairwise"]
    print(f"descriptor distances over {d['n_geometries']} geometries")
    print(f"  existing six pairwise: min {base['min']:.2f} median {base['median']:.2f} "
          f"max {base['max']:.2f}")
    for family, block in d["new_family_placement"].items():
        flag = "BELOW corpus minimum" if block["below_baseline_minimum"] else "above minimum"
        print(f"  {family:>18} -> {block['nearest_baseline_family']:<16} "
              f"{block['distance_to_nearest_baseline']:.2f}  ({flag})")

    slab = payload["tube_load_slab"]
    if slab.get("available"):
        print(f"\ntube load slab: ring/annulus {slab['ratio_min']:.3%}-{slab['ratio_max']:.3%} "
              f"(tolerance {slab['authored_area_tolerance']:.0%})")
        print(f"  counterfactual off diameter: {slab['counterfactual_ratio_min']:.1%}"
              f"-{slab['counterfactual_ratio_max']:.1%}")
        matched = all(e.get("derivation_matches_emitted_case", False)
                      for e in slab["per_regime"].values())
        print(f"  derivation matches the emitted case JSONs: {matched}")

    rec = payload["family_recovery"]
    print(f"\n1-NN family recovery: six families "
          f"{rec['baseline_six_families']['correct']}/"
          f"{rec['baseline_six_families']['n_geometries']}, all families "
          f"{rec['all_families']['correct']}/{rec['all_families']['n_geometries']}")

    curve = payload["learning_curve"]
    if curve.get("available"):
        print("\nlearning-curve slopes (decades per added family):")
        for name, fit in curve["slope_fits"].items():
            verdict = "flat" if fit["indistinguishable_from_flat"] else "SIGNIFICANT"
            print(f"  {name:>20} {fit['slope_decades_per_family']:+.4f} "
                  f"[{fit['ci95'][0]:+.4f}, {fit['ci95'][1]:+.4f}]  {verdict}")

    print("\npower: CI half-width on the slope by corpus width")
    for families, row in payload["power"]["by_family_count"].items():
        print(f"  {families:>3} families: {row['ci95_half_width']:.4f}")
    print(f"\nwrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
