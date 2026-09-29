# SPDX-License-Identifier: BSD-3-Clause
"""Learned mesh advisor tooling (ADR-0027).

Scripts put ``scripts/`` on ``sys.path`` and import ``advisor.<module>``; this
package module deliberately imports nothing, so ``advisor.paths`` and
``advisor.features`` stay free of numpy/torch.

Modules, by pipeline stage
--------------------------
Shared      ``paths`` (repository locations), ``features`` (the feature-column
            families).
Campaigns   ``run_batch``, ``regenerate_campaign``, ``rebuild_results``,
            ``promote_truth``. ``scripts/build_advisor_dataset.py`` then turns
            campaign rows into ``bench/advisor/dataset.csv``.
Dataset     ``dataset`` (CSV -> standardized tensors, per-head masks; re-exports
            ``csv_io``, ``splits`` and ``actions``), ``cost_labels``,
            ``geometry_features`` (offline exact-BRep descriptors).
Training    ``model`` (``AdvisorNet``), ``losses``, ``train``, ``prune``.
Evaluation  ``regret``, ``crossval``, ``evaluate``, ``calibration``,
            ``tolerance_selector``, ``learning_curve``, ``corpus_evidence``.
Export      ``export_onnx`` (ONNX graph + normalization/clamp/OOD artifacts +
            parity fixtures), with ``export_graph``, ``export_fixtures`` and
            ``export_parity``.
Reports     ``report`` (with ``report_common``, ``report_campaign``,
            ``report_external``, ``report_meshes``, ``report_network``),
            ``dashboard`` (with ``dashboard_charts``, ``dashboard_network``),
            ``figures``, ``plot_evaluation`` (with ``plot_evaluation_data``,
            ``plot_evaluation_draw``).
Research    ``capacity_sweep``, ``architecture_benchmark``,
            ``precision_benchmark`` -- measurements, never the shipped model.

``normalization.json`` and ``clamps.json`` written by training/export are the
single source of truth shared with the C++ inference module: a deployed model's
input columns and action layout are whatever those artifacts record.
"""

from __future__ import annotations

__all__ = [
    "paths", "features",
    "run_batch", "regenerate_campaign", "rebuild_results", "promote_truth",
    "dataset", "csv_io", "splits", "actions", "cost_labels", "geometry_features",
    "model", "losses", "train", "prune",
    "regret", "crossval", "evaluate", "calibration", "tolerance_selector",
    "learning_curve", "corpus_evidence",
    "export_onnx", "export_graph", "export_fixtures", "export_parity",
    "report", "report_common", "report_campaign", "report_external",
    "report_meshes", "report_network",
    "dashboard", "dashboard_charts", "dashboard_network",
    "figures", "plot_evaluation", "plot_evaluation_data", "plot_evaluation_draw",
    "capacity_sweep", "architecture_benchmark", "precision_benchmark",
]
