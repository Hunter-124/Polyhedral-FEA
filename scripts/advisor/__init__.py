# SPDX-License-Identifier: BSD-3-Clause
"""Learned mesh advisor training package (ADR-0027).

Modules
-------
``dataset``      CSV -> standardized tensors, per-head masks, part-hash split.
``model``        ``AdvisorNet`` multi-head MLP with categorical embeddings.
``prune``        accumulating worst-residual row pruning.
``train``        staged multi-objective training loop + LightGBM baseline.
``export_onnx``  ONNX export + normalization/clamp artifacts + parity check.

Every on-disk artifact produced here is specified by the advisor contracts
(C2, C4-C8); ``clamps.json`` and ``normalization.json`` are the single source
of truth shared with the C++ inference module.
"""

from __future__ import annotations

__all__ = ["dataset", "model", "prune", "train", "export_onnx"]
