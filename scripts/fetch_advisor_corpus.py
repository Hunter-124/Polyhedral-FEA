#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Fetch the CAD corpora the learned mesh advisor trains on (ADR-0027).

Only commercially clean sources are fetched by default. Non-commercial sources
(SimJEB, Fusion 360 Gallery) require --allow-noncommercial and land under a
separate ``nc/`` prefix so a shipped checkpoint can never be trained on them.

Everything lands in ``bench/geometries/corpus/`` which is gitignored; the
per-source licence record is written to ``bench/advisor/corpus_manifest.json``
and IS committed.

Hugging Face rate-limits anonymous IPs hard (HTTP 429) when pulling tens of
thousands of small files. Set ``HF_TOKEN`` (any free account works) or accept
the throttled path: this script retries with exponential backoff and
``snapshot_download`` resumes, so re-running is cheap and idempotent.

Usage:
    python scripts/fetch_advisor_corpus.py --source sfem
    python scripts/fetch_advisor_corpus.py --source sfem --shards 000 001 002
    python scripts/fetch_advisor_corpus.py --list
"""

from __future__ import annotations

import argparse
import json
import os
import random
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
CORPUS_DIR = REPO_ROOT / "bench" / "geometries" / "corpus"
MANIFEST = REPO_ROOT / "bench" / "advisor" / "corpus_manifest.json"

# Licence strings are the exact declared licence, not a paraphrase. `commercial`
# gates whether a source may contribute to a shipped model checkpoint.
SOURCES: dict[str, dict] = {
    "sfem": {
        "kind": "hf_dataset",
        "repo_id": "cmudrc/SFEM",
        "licence": "MIT",
        "commercial": True,
        "patterns": ["Step_Files/*", "README.md"],
        "shard_pattern": "Step_Files/{shard}/*",
        "note": (
            "~16k BrepGen STEP solids with FEniCSx linear-elastic fields, "
            "fixed_facet_mask and stochastic point loads (200/2000/20000 N)."
        ),
        "url": "https://huggingface.co/datasets/cmudrc/SFEM",
    },
    "mfcadpp": {
        "kind": "manual",
        "licence": "CC BY",
        "commercial": True,
        "note": (
            "59,665 PythonOCC-generated STEP B-reps with 24 face-level "
            "machining-feature labels. Single 1.5 GB ZIP behind a QUB DOI "
            "landing page; no direct API. Download by hand into "
            "bench/geometries/corpus/mfcadpp/."
        ),
        "url": (
            "https://pure.qub.ac.uk/en/datasets/"
            "mfcad-dataset-dataset-for-paper-hierarchical-cadnet-learning-from"
        ),
    },
    "abc": {
        "kind": "manual",
        "licence": "MIT (paper) / Onshape Terms of Use (per-model)",
        "commercial": False,  # requires per-model provenance review first
        "note": (
            "1M human-authored CAD models. Take ONE 10k STEP chunk "
            "(<=1.73 GB) and run the OCCT solid gate plus a licence-provenance "
            "review before any model trains on it. 106.65 GB for all STEP."
        ),
        "url": "https://deep-geometry.github.io/abc-dataset/",
    },
    "simjeb": {
        "kind": "manual",
        "licence": "ODC-By 1.0 database; CAD is GrabCAD non-commercial",
        "commercial": False,
        "note": (
            "381 real jet-engine brackets, closed STEP, four linear-static "
            "load cases with full nodal fields. EVALUATION LANE ONLY - the CAD "
            "is GrabCAD-derived and must never train a shipped checkpoint."
        ),
        "url": "https://simjeb.github.io/",
    },
}


def _record(name: str, spec: dict, dest: Path, status: str, n_files: int) -> None:
    MANIFEST.parent.mkdir(parents=True, exist_ok=True)
    data = {}
    if MANIFEST.exists():
        data = json.loads(MANIFEST.read_text(encoding="utf-8"))
    data[name] = {
        "licence": spec["licence"],
        "commercial_use_permitted": spec["commercial"],
        "url": spec["url"],
        "note": spec["note"],
        "local_path": str(dest.relative_to(REPO_ROOT)).replace("\\", "/"),
        "status": status,
        "files_present": n_files,
        "fetched_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }
    MANIFEST.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def fetch_hf(name: str, spec: dict, shards: list[str] | None, tries: int) -> int:
    from huggingface_hub import snapshot_download
    from huggingface_hub.errors import HfHubHTTPError

    dest = CORPUS_DIR / name
    dest.mkdir(parents=True, exist_ok=True)
    patterns = spec["patterns"]
    if shards:
        patterns = [spec["shard_pattern"].format(shard=s) for s in shards]
        patterns.append("README.md")

    token = os.environ.get("HF_TOKEN") or os.environ.get("HUGGING_FACE_HUB_TOKEN")
    # Anonymous pulls of many small files get 429'd; one worker plus backoff is
    # slow but finishes. A token lifts the limit and lets us parallelise.
    workers = 8 if token else 2

    for attempt in range(1, tries + 1):
        try:
            snapshot_download(
                repo_id=spec["repo_id"],
                repo_type="dataset",
                allow_patterns=patterns,
                local_dir=str(dest),
                max_workers=workers,
                token=token,
            )
            break
        except HfHubHTTPError as exc:
            if "429" not in str(exc) or attempt == tries:
                raise
            # snapshot_download resumes, so sleeping and retrying is free.
            delay = min(600.0, 20.0 * 2 ** (attempt - 1)) * (0.75 + 0.5 * random.random())
            print(
                f"  rate limited (attempt {attempt}/{tries}); "
                f"sleeping {delay:.0f}s then resuming",
                flush=True,
            )
            time.sleep(delay)

    n = sum(1 for _ in dest.rglob("*.step"))
    _record(name, spec, dest, "ok", n)
    return n


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source", choices=sorted(SOURCES), help="corpus to fetch")
    ap.add_argument("--shards", nargs="*", default=None,
                    help="for sharded HF datasets, fetch only these top-level "
                         "directories (e.g. 000 001) instead of everything")
    ap.add_argument("--tries", type=int, default=12,
                    help="rate-limit retry attempts (default 12)")
    ap.add_argument("--allow-noncommercial", action="store_true",
                    help="permit fetching sources whose licence forbids "
                         "commercial use; they land under an nc/ prefix")
    ap.add_argument("--list", action="store_true", help="list sources and exit")
    args = ap.parse_args(argv)

    if args.list or not args.source:
        for name, spec in sorted(SOURCES.items()):
            flag = "commercial-ok" if spec["commercial"] else "NON-COMMERCIAL"
            print(f"{name:10s}  {flag:14s}  {spec['licence']}")
            print(f"            {spec['note']}")
            print(f"            {spec['url']}")
        return 0

    spec = SOURCES[args.source]
    if not spec["commercial"] and not args.allow_noncommercial:
        print(f"refusing: {args.source} is {spec['licence']}. "
              f"Re-run with --allow-noncommercial to place it in the isolated "
              f"evaluation lane.", file=sys.stderr)
        return 2

    if spec["kind"] == "manual":
        dest = CORPUS_DIR / args.source
        dest.mkdir(parents=True, exist_ok=True)
        n = sum(1 for _ in dest.rglob("*.st*p"))
        print(f"{args.source}: manual download required -> {spec['url']}")
        print(f"  place files under {dest}")
        print(f"  {spec['note']}")
        print(f"  currently present: {n} STEP files")
        _record(args.source, spec, dest, "manual", n)
        return 0

    n = fetch_hf(args.source, spec, args.shards, args.tries)
    print(f"{args.source}: {n} STEP files under {CORPUS_DIR / args.source}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
