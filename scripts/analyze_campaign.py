#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Campaign feedback-loop analysis: Pareto frontiers + config rankings.

Works on partial or finished campaigns. Reads:
  bench/campaigns/<name>/results.jsonl
  bench/campaigns/<name>/campaign.json   (optional — score weights)
  bench/campaigns/<name>/checkpoint.json (optional — state / survivors)

Writes:
  bench/campaigns/<name>/PARETO.md
  bench/campaigns/<name>/PARETO.json

Scoring matches apps/testlab/main.cpp scalar_score:
  s_mesh  = 1 / (1 + mesh_ms / 1000)
  s_solve = 1 / (1 + solve_ms / 1000)
  score   = w_acc * accuracy + w_mesh * s_mesh + w_solve * s_solve

Usage (from repo root):
  python3 scripts/analyze_campaign.py settings-frontier-1
  python3 scripts/analyze_campaign.py smoke
  python3 scripts/analyze_campaign.py bench/campaigns/settings-frontier-1
  python3 scripts/analyze_campaign.py --all
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Optional

ROOT = Path(__file__).resolve().parents[1]
CAMPAIGNS = ROOT / "bench" / "campaigns"

DEFAULT_WEIGHTS = {"accuracy": 0.5, "mesh_ms": 0.25, "solve_ms": 0.25}


# ── loaders ──────────────────────────────────────────────────────────────────


def _load_json(path: Path) -> Optional[dict]:
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError) as e:
        print(f"warning: could not parse {path}: {e}", file=sys.stderr)
        return None


def load_results(path: Path) -> list[dict]:
    """Load results.jsonl; skip blank / corrupt lines (partial-safe)."""
    rows: list[dict] = []
    if not path.is_file():
        return rows
    with path.open(encoding="utf-8") as f:
        for i, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError as e:
                print(f"warning: skip corrupt line {i} in {path.name}: {e}", file=sys.stderr)
    return rows


def resolve_campaign_dir(name_or_path: str) -> Path:
    p = Path(name_or_path)
    if p.is_dir() and (
        (p / "results.jsonl").exists() or (p / "campaign.json").exists()
    ):
        return p.resolve()
    cand = CAMPAIGNS / name_or_path
    if cand.is_dir():
        return cand.resolve()
    raise SystemExit(f"campaign not found: {name_or_path} (tried {cand})")


def weights_from_campaign(camp: Optional[dict]) -> dict[str, float]:
    w = dict(DEFAULT_WEIGHTS)
    if not camp:
        return w
    score = camp.get("score") or {}
    weights = score.get("weights") or {}
    w["accuracy"] = float(weights.get("accuracy", w["accuracy"]))
    w["mesh_ms"] = float(weights.get("mesh_ms", w["mesh_ms"]))
    w["solve_ms"] = float(weights.get("solve_ms", w["solve_ms"]))
    return w


# ── scoring ──────────────────────────────────────────────────────────────────


def accuracy_of(row: dict) -> float:
    if row.get("status") != "ok":
        return 0.0
    acc = row.get("accuracy") or {}
    if isinstance(acc, dict) and "score" in acc:
        return float(acc["score"])
    if isinstance(acc, dict) and "rel_err" in acc:
        # Reconstruct like testlab when score missing (tol default 0.05).
        return 1.0 / (1.0 + abs(float(acc["rel_err"])) / 0.05)
    return 0.0


def soft_time_score(ms: float) -> float:
    return 1.0 / (1.0 + float(ms) / 1000.0)


def scalar_score(row: dict, weights: dict[str, float]) -> float:
    if row.get("status") != "ok":
        # Failures still pay mesh/solve time if recorded, but accuracy=0.
        mesh_ms = float(row.get("mesh_ms") or 0.0)
        solve_ms = float(row.get("solve_ms") or 0.0)
        return (
            weights["accuracy"] * 0.0
            + weights["mesh_ms"] * soft_time_score(mesh_ms)
            + weights["solve_ms"] * soft_time_score(solve_ms)
        )
    acc = accuracy_of(row)
    mesh_ms = float(row.get("mesh_ms") or 0.0)
    solve_ms = float(row.get("solve_ms") or 0.0)
    return (
        weights["accuracy"] * acc
        + weights["mesh_ms"] * soft_time_score(mesh_ms)
        + weights["solve_ms"] * soft_time_score(solve_ms)
    )


def total_ms(row: dict) -> float:
    return float(row.get("mesh_ms") or 0.0) + float(row.get("solve_ms") or 0.0)


def rel_err_of(row: dict) -> Optional[float]:
    if row.get("status") != "ok":
        return None
    acc = row.get("accuracy") or {}
    if isinstance(acc, dict) and "rel_err" in acc:
        return float(acc["rel_err"])
    return None


# ── geom class bucketing ─────────────────────────────────────────────────────


def geom_bucket(row: dict) -> str:
    """Coarse condition class for presets (interfaces.md §3 geom_class)."""
    g = row.get("geom_class") or {}
    thin = bool(g.get("thin", False))
    curved = float(g.get("curved_frac") or 0.0)
    if thin:
        return "thin_wall"
    if curved >= 0.25:
        return "curved"
    if curved >= 0.05:
        return "mild_curve"
    return "prismatic"


def config_label(cfg: dict) -> str:
    if not cfg:
        return "{}"
    keys = sorted(cfg.keys())
    parts = []
    for k in keys:
        v = cfg[k]
        if isinstance(v, float):
            parts.append(f"{k}={v:g}")
        else:
            parts.append(f"{k}={v}")
    return ", ".join(parts)


# ── aggregation ──────────────────────────────────────────────────────────────


@dataclass
class CfgAgg:
    cfg_id: str
    config: dict = field(default_factory=dict)
    scores: list[float] = field(default_factory=list)
    accuracies: list[float] = field(default_factory=list)
    total_mss: list[float] = field(default_factory=list)
    mesh_mss: list[float] = field(default_factory=list)
    solve_mss: list[float] = field(default_factory=list)
    n_ok: int = 0
    n_fail: int = 0
    parts: set[str] = field(default_factory=set)
    tiers: set[int] = field(default_factory=set)
    by_part: dict[str, list[float]] = field(default_factory=lambda: defaultdict(list))

    def mean_score(self) -> float:
        return sum(self.scores) / len(self.scores) if self.scores else 0.0

    def mean_accuracy(self) -> float:
        return sum(self.accuracies) / len(self.accuracies) if self.accuracies else 0.0

    def mean_total_ms(self) -> float:
        return sum(self.total_mss) / len(self.total_mss) if self.total_mss else 0.0

    def ok_rate(self) -> float:
        n = self.n_ok + self.n_fail
        return self.n_ok / n if n else 0.0


def aggregate_configs(rows: list[dict], weights: dict[str, float]) -> dict[str, CfgAgg]:
    out: dict[str, CfgAgg] = {}
    for r in rows:
        cid = r.get("cfg_id") or "unknown"
        if cid not in out:
            out[cid] = CfgAgg(cfg_id=cid, config=dict(r.get("config") or {}))
        a = out[cid]
        if r.get("config"):
            a.config = dict(r["config"])
        sc = scalar_score(r, weights)
        a.scores.append(sc)
        a.accuracies.append(accuracy_of(r))
        a.total_mss.append(total_ms(r))
        a.mesh_mss.append(float(r.get("mesh_ms") or 0.0))
        a.solve_mss.append(float(r.get("solve_ms") or 0.0))
        a.parts.add(str(r.get("part") or "?"))
        a.tiers.add(int(r.get("tier") or 0))
        part = str(r.get("part") or "?")
        a.by_part[part].append(sc)
        if r.get("status") == "ok":
            a.n_ok += 1
        else:
            a.n_fail += 1
    return out


def mean_map(values: list[float]) -> float:
    return sum(values) / len(values) if values else 0.0


# ── Pareto ───────────────────────────────────────────────────────────────────


@dataclass
class ParetoPoint:
    key: str  # cfg_id or cfg_id@part
    cfg_id: str
    config: dict
    accuracy: float
    total_ms: float
    mesh_ms: float
    solve_ms: float
    score: float
    part: Optional[str] = None
    geom: Optional[str] = None
    n: int = 1
    status_ok: bool = True


def pareto_front(points: list[ParetoPoint]) -> list[ParetoPoint]:
    """Maximize accuracy, minimize total_ms. Non-dominated subset."""
    if not points:
        return []
    # Sort for stable deterministic output: higher accuracy first, then lower cost.
    ordered = sorted(points, key=lambda p: (-p.accuracy, p.total_ms, p.key))
    front: list[ParetoPoint] = []
    for p in ordered:
        dominated = False
        for q in ordered:
            if q.key == p.key:
                continue
            # q dominates p if q is at least as good on both and better on one.
            if (q.accuracy >= p.accuracy and q.total_ms <= p.total_ms) and (
                q.accuracy > p.accuracy or q.total_ms < p.total_ms
            ):
                dominated = True
                break
        if not dominated:
            front.append(p)
    # Keep front sorted accuracy desc, then cost asc.
    front.sort(key=lambda p: (-p.accuracy, p.total_ms, p.key))
    return front


def points_from_rows(
    rows: list[dict],
    weights: dict[str, float],
    *,
    ok_only: bool = True,
) -> list[ParetoPoint]:
    pts: list[ParetoPoint] = []
    for r in rows:
        if ok_only and r.get("status") != "ok":
            continue
        cid = str(r.get("cfg_id") or "unknown")
        part = str(r.get("part") or "?")
        pts.append(
            ParetoPoint(
                key=f"{cid}|{part}|{r.get('tier', 0)}",
                cfg_id=cid,
                config=dict(r.get("config") or {}),
                accuracy=accuracy_of(r),
                total_ms=total_ms(r),
                mesh_ms=float(r.get("mesh_ms") or 0.0),
                solve_ms=float(r.get("solve_ms") or 0.0),
                score=scalar_score(r, weights),
                part=part,
                geom=geom_bucket(r),
                status_ok=r.get("status") == "ok",
            )
        )
    return pts


def points_per_cfg(
    aggs: dict[str, CfgAgg], weights: dict[str, float], *, ok_only: bool = True
) -> list[ParetoPoint]:
    """One Pareto point per config using mean accuracy vs mean total_ms."""
    pts: list[ParetoPoint] = []
    for cid, a in aggs.items():
        if ok_only and a.n_ok == 0:
            continue
        # Mean over all runs (fails pull accuracy down if included in lists).
        # For cost/accuracy frontier prefer ok-only means when available.
        if ok_only and a.n_ok > 0:
            # Recompute from stored lists is approximate; use means of all and
            # weight by presence — accuracies already 0 for fails.
            acc = a.mean_accuracy()
            tms = a.mean_total_ms()
            mesh = mean_map(a.mesh_mss)
            solve = mean_map(a.solve_mss)
        else:
            acc = a.mean_accuracy()
            tms = a.mean_total_ms()
            mesh = mean_map(a.mesh_mss)
            solve = mean_map(a.solve_mss)
        pts.append(
            ParetoPoint(
                key=cid,
                cfg_id=cid,
                config=a.config,
                accuracy=acc,
                total_ms=tms,
                mesh_ms=mesh,
                solve_ms=solve,
                score=a.mean_score(),
                n=a.n_ok + a.n_fail,
                status_ok=a.n_fail == 0,
            )
        )
    return pts


# ── recommendations ──────────────────────────────────────────────────────────


def factor_breakdown(aggs: dict[str, CfgAgg]) -> dict[str, dict[str, dict[str, float]]]:
    """Per config-key (mesher, element_tendency, …) mean score and ok rate."""
    buckets: dict[str, dict[Any, list[tuple[float, int, int]]]] = defaultdict(
        lambda: defaultdict(list)
    )
    for a in aggs.values():
        cfg = a.config or {}
        for k, v in cfg.items():
            # JSON keys only; skip nested.
            if isinstance(v, (dict, list)):
                continue
            key = v if not isinstance(v, float) else round(v, 6)
            buckets[k][key].append((a.mean_score(), a.n_ok, a.n_fail))
    out: dict[str, dict[str, dict[str, float]]] = {}
    for factor, levels in buckets.items():
        out[factor] = {}
        for level, samples in levels.items():
            scores = [s[0] for s in samples]
            n_ok = sum(s[1] for s in samples)
            n_fail = sum(s[2] for s in samples)
            n = n_ok + n_fail
            out[factor][str(level)] = {
                "mean_score": mean_map(scores),
                "n_configs": float(len(samples)),
                "n_runs": float(n),
                "ok_rate": (n_ok / n) if n else 0.0,
            }
    return out


def suggest_defaults(
    aggs: dict[str, CfgAgg],
    rows: list[dict],
    *,
    finished: bool,
    min_ok_rate: float = 0.85,
    min_runs: int = 12,
) -> dict[str, Any]:
    """Propose mesher / element_tendency / feature_refine defaults.

    Does NOT apply them — callers document only unless finished + solid ok rate.
    `per_condition` is filled by the caller via per_condition_presets().
    """
    n = len(rows)
    n_ok = sum(1 for r in rows if r.get("status") == "ok")
    overall_ok = n_ok / n if n else 0.0
    solid = finished and overall_ok >= min_ok_rate and n >= min_runs

    ranked = sorted(aggs.values(), key=lambda a: (-a.mean_score(), a.cfg_id))
    top = ranked[0] if ranked else None
    factors = factor_breakdown(aggs)

    def best_level(factor: str) -> Optional[tuple[str, dict]]:
        levels = factors.get(factor) or {}
        if not levels:
            return None
        items = sorted(
            levels.items(),
            key=lambda kv: (-kv[1]["mean_score"], -kv[1]["ok_rate"], kv[0]),
        )
        return items[0] if items else None

    recs: list[dict[str, Any]] = []
    for factor in ("mesher", "element_tendency", "feature_refine", "order"):
        bl = best_level(factor)
        if not bl:
            continue
        name, stats = bl
        recs.append(
            {
                "knob": factor,
                "suggested": _parse_level(name),
                "mean_score": stats["mean_score"],
                "ok_rate": stats["ok_rate"],
                "n_runs": int(stats["n_runs"]),
                "n_configs": int(stats["n_configs"]),
            }
        )

    return {
        "apply_code_defaults": solid,
        "reason": (
            "Campaign finished with solid ok-rate; defaults may be updated."
            if solid
            else (
                "Partial or insufficient data — document recommendations only; "
                "do not change product defaults yet."
            )
        ),
        "overall_ok_rate": overall_ok,
        "n_runs": n,
        "finished": finished,
        "global_top_cfg": (
            {
                "cfg_id": top.cfg_id,
                "config": top.config,
                "mean_score": top.mean_score(),
                "mean_accuracy": top.mean_accuracy(),
                "ok_rate": top.ok_rate(),
                "n_ok": top.n_ok,
                "n_fail": top.n_fail,
            }
            if top
            else None
        ),
        "knob_suggestions": recs,
        "factor_breakdown": factors,
        "per_condition": {},  # filled by write_reports()
    }


def _parse_level(name: str) -> Any:
    if name in ("True", "False"):
        return name == "True"
    try:
        if "." in name or "e" in name.lower():
            return float(name)
        return int(name)
    except ValueError:
        return name


def per_condition_presets(
    rows: list[dict], weights: dict[str, float]
) -> dict[str, Any]:
    """Best config factors per geom_class and per part."""
    groups: dict[str, list[dict]] = defaultdict(list)
    for r in rows:
        groups[f"geom:{geom_bucket(r)}"].append(r)
        groups[f"part:{r.get('part', '?')}"].append(r)

    out: dict[str, Any] = {}
    for gname, grows in sorted(groups.items()):
        # Aggregate by cfg_id within group.
        by_cfg: dict[str, list[float]] = defaultdict(list)
        cfg_map: dict[str, dict] = {}
        ok_counts: dict[str, list[int]] = defaultdict(lambda: [0, 0])
        for r in grows:
            cid = str(r.get("cfg_id") or "?")
            by_cfg[cid].append(scalar_score(r, weights))
            cfg_map[cid] = dict(r.get("config") or {})
            if r.get("status") == "ok":
                ok_counts[cid][0] += 1
            else:
                ok_counts[cid][1] += 1
        ranked = sorted(
            by_cfg.items(),
            key=lambda kv: (-mean_map(kv[1]), kv[0]),
        )
        top_n = []
        for cid, scores in ranked[:5]:
            n_ok, n_fail = ok_counts[cid]
            top_n.append(
                {
                    "cfg_id": cid,
                    "config": cfg_map.get(cid, {}),
                    "mean_score": mean_map(scores),
                    "n_runs": len(scores),
                    "ok_rate": n_ok / (n_ok + n_fail) if (n_ok + n_fail) else 0.0,
                }
            )
        # Factor winners within group (ok rows preferred for accuracy).
        factor_best: dict[str, Any] = {}
        for factor in ("mesher", "element_tendency", "feature_refine"):
            level_scores: dict[Any, list[float]] = defaultdict(list)
            for r in grows:
                cfg = r.get("config") or {}
                if factor not in cfg:
                    continue
                level_scores[cfg[factor]].append(scalar_score(r, weights))
            if not level_scores:
                continue
            best = max(level_scores.items(), key=lambda kv: (mean_map(kv[1]), str(kv[0])))
            factor_best[factor] = {
                "value": best[0],
                "mean_score": mean_map(best[1]),
                "n": len(best[1]),
            }
        out[gname] = {
            "n_runs": len(grows),
            "ok_rate": sum(1 for r in grows if r.get("status") == "ok") / len(grows),
            "top_configs": top_n,
            "factor_best": factor_best,
        }
    return out


# ── report writers ───────────────────────────────────────────────────────────


def _fmt_pct(x: float) -> str:
    return f"{100.0 * x:.1f}%"


def _fmt_ms(x: float) -> str:
    if x >= 10000:
        return f"{x / 1000:.1f}s"
    if x >= 1000:
        return f"{x / 1000:.2f}s"
    return f"{x:.0f}ms"


def write_reports(
    camp_dir: Path,
    *,
    name: str,
    rows: list[dict],
    weights: dict[str, float],
    checkpoint: Optional[dict],
    campaign: Optional[dict],
) -> dict[str, Any]:
    finished = bool(checkpoint and checkpoint.get("state") == "finished")
    state = (checkpoint or {}).get("state", "unknown")
    tier = (checkpoint or {}).get("tier")
    completed = (checkpoint or {}).get("completed_runs", len(rows))
    survivors = (checkpoint or {}).get("survivors") or []

    n_ok = sum(1 for r in rows if r.get("status") == "ok")
    n_fail = len(rows) - n_ok
    ok_rate = n_ok / len(rows) if rows else 0.0

    aggs = aggregate_configs(rows, weights)
    ranked = sorted(aggs.values(), key=lambda a: (-a.mean_score(), a.cfg_id))

    # Pareto: global (mean per cfg) and per-part (individual ok runs).
    global_pts = points_per_cfg(aggs, weights, ok_only=True)
    global_front = pareto_front(global_pts)

    by_part_rows: dict[str, list[dict]] = defaultdict(list)
    by_geom_rows: dict[str, list[dict]] = defaultdict(list)
    for r in rows:
        by_part_rows[str(r.get("part") or "?")].append(r)
        by_geom_rows[geom_bucket(r)].append(r)

    per_part_front: dict[str, list[ParetoPoint]] = {}
    for part, grows in by_part_rows.items():
        # Aggregate by cfg within part (may have multi-tier).
        part_aggs = aggregate_configs(grows, weights)
        pts = points_per_cfg(part_aggs, weights, ok_only=True)
        for p in pts:
            p.part = part
        per_part_front[part] = pareto_front(pts)

    per_geom_front: dict[str, list[ParetoPoint]] = {}
    for geom, grows in by_geom_rows.items():
        g_aggs = aggregate_configs(grows, weights)
        pts = points_per_cfg(g_aggs, weights, ok_only=True)
        for p in pts:
            p.geom = geom
        per_geom_front[geom] = pareto_front(pts)

    suggestions = suggest_defaults(aggs, rows, finished=finished)
    suggestions["per_condition"] = per_condition_presets(rows, weights)
    # Fix knob scores using actual weights in factor_breakdown already from aggs.

    # Expected grid size for completeness note.
    expected_note = ""
    if campaign and "grid" in campaign:
        grid = campaign["grid"]
        n_cfg = 1
        for v in grid.values():
            if isinstance(v, list):
                n_cfg *= max(len(v), 1)
        n_parts = len(campaign.get("parts") or [])
        n_tiers = len(campaign.get("tiers") or [1])
        # Tier 0 is full factorial; higher tiers are subset — report tier-0 bound.
        expected_t0 = n_cfg * n_parts
        expected_note = (
            f"Grid: {n_cfg} configs × {n_parts} parts = {expected_t0} tier-0 runs "
            f"({n_tiers} tiers planned)."
        )

    summary: dict[str, Any] = {
        "campaign": name,
        "generated_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "partial": not finished,
        "checkpoint": {
            "state": state,
            "tier": tier,
            "completed_runs": completed,
            "n_survivors": len(survivors),
            "survivors": survivors if finished else survivors[:12],
        },
        "weights": weights,
        "n_result_lines": len(rows),
        "n_ok": n_ok,
        "n_fail": n_fail,
        "ok_rate": ok_rate,
        "n_configs": len(aggs),
        "expected_note": expected_note,
        "ranking": [
            {
                "rank": i + 1,
                "cfg_id": a.cfg_id,
                "config": a.config,
                "mean_score": a.mean_score(),
                "mean_accuracy": a.mean_accuracy(),
                "mean_total_ms": a.mean_total_ms(),
                "mean_mesh_ms": mean_map(a.mesh_mss),
                "mean_solve_ms": mean_map(a.solve_mss),
                "ok_rate": a.ok_rate(),
                "n_ok": a.n_ok,
                "n_fail": a.n_fail,
                "parts": sorted(a.parts),
                "tiers": sorted(a.tiers),
            }
            for i, a in enumerate(ranked)
        ],
        "pareto_global": [_point_dict(p) for p in global_front],
        "pareto_by_part": {k: [_point_dict(p) for p in v] for k, v in sorted(per_part_front.items())},
        "pareto_by_geom_class": {
            k: [_point_dict(p) for p in v] for k, v in sorted(per_geom_front.items())
        },
        "recommendations": suggestions,
    }

    # ── JSON ──────────────────────────────────────────────────────────────
    json_path = camp_dir / "PARETO.json"
    json_path.write_text(json.dumps(summary, indent=2, sort_keys=False) + "\n", encoding="utf-8")

    # ── Markdown ──────────────────────────────────────────────────────────
    md = _render_markdown(summary)
    md_path = camp_dir / "PARETO.md"
    md_path.write_text(md, encoding="utf-8")

    return summary


def _point_dict(p: ParetoPoint) -> dict[str, Any]:
    d: dict[str, Any] = {
        "cfg_id": p.cfg_id,
        "config": p.config,
        "accuracy": p.accuracy,
        "total_ms": p.total_ms,
        "mesh_ms": p.mesh_ms,
        "solve_ms": p.solve_ms,
        "score": p.score,
        "n": p.n,
    }
    if p.part is not None:
        d["part"] = p.part
    if p.geom is not None:
        d["geom_class"] = p.geom
    return d


def _render_markdown(s: dict[str, Any]) -> str:
    lines: list[str] = []
    partial = s["partial"]
    state = s["checkpoint"]["state"]
    badge = "PARTIAL" if partial else "FINISHED"
    lines.append(f"# Pareto analysis — `{s['campaign']}`")
    lines.append("")
    lines.append(f"_Generated {s['generated_utc']} · campaign state: **{state}** ({badge})_")
    lines.append("")
    if partial:
        lines.append(
            "> **Partial results.** Re-run `python3 scripts/analyze_campaign.py "
            f"{s['campaign']}` when the campaign finishes to refresh this report."
        )
        lines.append("")

    lines.append("## Summary")
    lines.append("")
    lines.append(f"| Field | Value |")
    lines.append(f"|---|---|")
    lines.append(f"| Result lines | {s['n_result_lines']} |")
    lines.append(f"| OK / fail | {s['n_ok']} / {s['n_fail']} (ok rate {_fmt_pct(s['ok_rate'])}) |")
    lines.append(f"| Distinct configs | {s['n_configs']} |")
    lines.append(
        f"| Checkpoint | tier={s['checkpoint'].get('tier')}, "
        f"completed_runs={s['checkpoint'].get('completed_runs')}, "
        f"survivors={s['checkpoint'].get('n_survivors')} |"
    )
    w = s["weights"]
    lines.append(
        f"| Score weights | accuracy={w['accuracy']}, "
        f"mesh_ms={w['mesh_ms']}, solve_ms={w['solve_ms']} |"
    )
    if s.get("expected_note"):
        lines.append(f"| Grid | {s['expected_note']} |")
    lines.append("")
    lines.append(
        "Composite score (matches `polymesh_testlab` successive-halving):  \n"
        "`score = w_acc·accuracy + w_mesh/(1+mesh_ms/1000) + w_solve/(1+solve_ms/1000)`."
    )
    lines.append("")
    lines.append(
        "Pareto axes: **maximize** mean accuracy, **minimize** mean "
        "`mesh_ms + solve_ms` (ok runs contribute; configs with zero ok runs excluded)."
    )
    lines.append("")

    # Ranking
    lines.append("## Config ranking (weighted mean score)")
    lines.append("")
    lines.append(
        "| Rank | cfg_id | score | accuracy | total time | ok | config |"
    )
    lines.append("|---:|---|---:|---:|---:|---:|---|")
    for row in s["ranking"][:25]:
        cfg = config_label(row["config"])
        lines.append(
            f"| {row['rank']} | `{row['cfg_id']}` | {row['mean_score']:.4f} | "
            f"{row['mean_accuracy']:.3f} | {_fmt_ms(row['mean_total_ms'])} | "
            f"{_fmt_pct(row['ok_rate'])} | {cfg} |"
        )
    if len(s["ranking"]) > 25:
        lines.append("")
        lines.append(f"_… {len(s['ranking']) - 25} more configs in `PARETO.json`._")
    lines.append("")

    # Global Pareto
    lines.append("## Global Pareto frontier (mean accuracy vs mean total time)")
    lines.append("")
    if not s["pareto_global"]:
        lines.append("_No ok runs yet — frontier empty._")
    else:
        lines.append("| cfg_id | accuracy | total time | score | config |")
        lines.append("|---|---:|---:|---:|---|")
        for p in s["pareto_global"]:
            lines.append(
                f"| `{p['cfg_id']}` | {p['accuracy']:.3f} | {_fmt_ms(p['total_ms'])} | "
                f"{p['score']:.4f} | {config_label(p['config'])} |"
            )
    lines.append("")

    # Per part
    lines.append("## Pareto by part")
    lines.append("")
    for part, front in s["pareto_by_part"].items():
        lines.append(f"### `{part}`")
        lines.append("")
        if not front:
            lines.append("_No ok runs._")
            lines.append("")
            continue
        lines.append("| cfg_id | accuracy | total time | score | config |")
        lines.append("|---|---:|---:|---:|---|")
        for p in front:
            lines.append(
                f"| `{p['cfg_id']}` | {p['accuracy']:.3f} | {_fmt_ms(p['total_ms'])} | "
                f"{p['score']:.4f} | {config_label(p['config'])} |"
            )
        lines.append("")

    # Per geom class
    lines.append("## Pareto by geometric class")
    lines.append("")
    lines.append(
        "Buckets from `geom_class`: `prismatic` (curved_frac < 0.05), "
        "`mild_curve` [0.05, 0.25), `curved` ≥ 0.25, `thin_wall` if thin."
    )
    lines.append("")
    for geom, front in s["pareto_by_geom_class"].items():
        lines.append(f"### `{geom}`")
        lines.append("")
        if not front:
            lines.append("_No ok runs._")
            lines.append("")
            continue
        lines.append("| cfg_id | accuracy | total time | score | config |")
        lines.append("|---|---:|---:|---:|---|")
        for p in front:
            lines.append(
                f"| `{p['cfg_id']}` | {p['accuracy']:.3f} | {_fmt_ms(p['total_ms'])} | "
                f"{p['score']:.4f} | {config_label(p['config'])} |"
            )
        lines.append("")

    # Recommendations
    rec = s["recommendations"]
    lines.append("## Default-knob recommendations")
    lines.append("")
    lines.append(f"**Apply code defaults?** `{rec['apply_code_defaults']}` — {rec['reason']}")
    lines.append("")
    lines.append(
        f"Overall ok-rate {_fmt_pct(rec['overall_ok_rate'])} "
        f"over {rec['n_runs']} runs; finished={rec['finished']}."
    )
    lines.append("")
    if rec.get("global_top_cfg"):
        t = rec["global_top_cfg"]
        lines.append(
            f"Top config overall: `{t['cfg_id']}` score={t['mean_score']:.4f} "
            f"({config_label(t['config'])})."
        )
        lines.append("")

    lines.append("### Factor-level winners (mean config score)")
    lines.append("")
    lines.append("| Knob | Best value | mean score | ok rate | n runs |")
    lines.append("|---|---|---:|---:|---:|")
    for k in rec.get("knob_suggestions") or []:
        lines.append(
            f"| `{k['knob']}` | `{k['suggested']}` | {k['mean_score']:.4f} | "
            f"{_fmt_pct(k['ok_rate'])} | {k['n_runs']} |"
        )
    lines.append("")

    # Full factor table
    fb = rec.get("factor_breakdown") or {}
    if fb:
        lines.append("### Full factor breakdown")
        lines.append("")
        for factor in sorted(fb.keys()):
            lines.append(f"**{factor}**")
            lines.append("")
            lines.append("| value | mean score | ok rate | n configs | n runs |")
            lines.append("|---|---:|---:|---:|---:|")
            levels = sorted(
                fb[factor].items(),
                key=lambda kv: (-kv[1]["mean_score"], kv[0]),
            )
            for val, st in levels:
                lines.append(
                    f"| `{val}` | {st['mean_score']:.4f} | {_fmt_pct(st['ok_rate'])} | "
                    f"{int(st['n_configs'])} | {int(st['n_runs'])} |"
                )
            lines.append("")

    # Per-condition presets
    pc = rec.get("per_condition") or {}
    if pc:
        lines.append("### Per-condition presets (for feedback-loop)")
        lines.append("")
        lines.append(
            "Use these as *candidate* presets once the campaign is finished with "
            "solid ok rates. Product defaults stay unchanged on partial data."
        )
        lines.append("")
        for gname, info in sorted(pc.items()):
            fb_g = info.get("factor_best") or {}
            bits = ", ".join(
                f"{k}=`{v['value']}` (score {v['mean_score']:.3f})"
                for k, v in sorted(fb_g.items())
            )
            lines.append(
                f"- **{gname}** — n={info['n_runs']}, ok={_fmt_pct(info['ok_rate'])}"
                + (f"; best: {bits}" if bits else "")
            )
        lines.append("")

    lines.append("## How to re-run")
    lines.append("")
    lines.append("```bash")
    lines.append(f"python3 scripts/analyze_campaign.py {s['campaign']}")
    lines.append("```")
    lines.append("")
    lines.append(
        "Safe on live campaigns: reads `results.jsonl` append-only; does not "
        "touch the runner or checkpoint state."
    )
    lines.append("")
    return "\n".join(lines)


# ── CLI ──────────────────────────────────────────────────────────────────────


def analyze_one(name_or_path: str) -> dict[str, Any]:
    camp_dir = resolve_campaign_dir(name_or_path)
    name = camp_dir.name
    rows = load_results(camp_dir / "results.jsonl")
    campaign = _load_json(camp_dir / "campaign.json")
    checkpoint = _load_json(camp_dir / "checkpoint.json")
    weights = weights_from_campaign(campaign)
    if not rows:
        print(f"{name}: no results yet (empty or missing results.jsonl)")
    summary = write_reports(
        camp_dir,
        name=name,
        rows=rows,
        weights=weights,
        checkpoint=checkpoint,
        campaign=campaign,
    )
    print(
        f"{name}: {summary['n_result_lines']} rows, "
        f"ok_rate={summary['ok_rate']:.1%}, state={summary['checkpoint']['state']}, "
        f"configs={summary['n_configs']}, "
        f"pareto={len(summary['pareto_global'])} → "
        f"{camp_dir / 'PARETO.md'}"
    )
    if summary["recommendations"].get("knob_suggestions"):
        tops = ", ".join(
            f"{k['knob']}={k['suggested']!r}"
            for k in summary["recommendations"]["knob_suggestions"][:4]
        )
        print(f"  knobs (provisional): {tops}")
        print(
            f"  apply_code_defaults={summary['recommendations']['apply_code_defaults']}"
        )
    return summary


def main(argv: Optional[list[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument(
        "campaigns",
        nargs="*",
        help="Campaign name under bench/campaigns/ or path to campaign dir",
    )
    ap.add_argument(
        "--all",
        action="store_true",
        help="Analyze every subdirectory of bench/campaigns/ that has results.jsonl",
    )
    args = ap.parse_args(argv)

    targets: list[str] = list(args.campaigns)
    if args.all:
        if not CAMPAIGNS.is_dir():
            print(f"no campaigns dir at {CAMPAIGNS}", file=sys.stderr)
            return 1
        for d in sorted(CAMPAIGNS.iterdir()):
            if d.is_dir() and (d / "results.jsonl").is_file():
                targets.append(str(d))
    if not targets:
        ap.print_help()
        return 2

    for t in targets:
        analyze_one(t)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
