# SPDX-License-Identifier: BSD-3-Clause
"""Repository-relative locations shared by the advisor tooling.

Pure constants: importing this module has no side effects. Every value is an
absolute :class:`pathlib.Path` resolved from this file's own location, so the
scripts work from any working directory.
"""

from __future__ import annotations

from pathlib import Path

#: Repository root (``scripts/advisor/paths.py`` -> two parents up).
REPO_ROOT: Path = Path(__file__).resolve().parents[2]
#: Advisor artifacts: dataset, normalization/clamps, checkpoints, reports.
ADVISOR_DIR: Path = REPO_ROOT / "bench" / "advisor"
#: One directory per campaign, each holding ``results.jsonl`` and ``runs/``.
CAMPAIGNS_DIR: Path = REPO_ROOT / "bench" / "campaigns"
#: Hand-authored fixture parts and their ``*.case.json``.
FIXTURE_PARTS_DIR: Path = REPO_ROOT / "tests" / "fixtures" / "parts"
#: Procedural corpus (``scripts/gen_primitive_corpus.py``): STEP + case JSON.
CORPUS_PRIMITIVES_DIR: Path = REPO_ROOT / "bench" / "geometries" / "corpus" / "primitives"
#: Reference truth consumed when re-deriving accuracy.
REFERENCE_DIR: Path = REPO_ROOT / "bench" / "reference"
