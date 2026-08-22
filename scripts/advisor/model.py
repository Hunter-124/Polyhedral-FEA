#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""``AdvisorNet``: the multi-head mesh-advisor MLP (contracts C6 and C8).

Architecture
------------
The ONNX graph takes a single standardized ``features`` tensor ``[batch, D]``
(C6), so the categorical embedding lookups happen *inside* the network: the
``order_idx`` / ``mesher_idx`` columns are passthrough (mean 0, std 1) floats
that are rounded, clamped and gathered against small embedding tables. The
remaining ``D - 2`` continuous columns are concatenated with the two 4-dim
embeddings to form the trunk input of width ``D_eff = (D - 2) + 8``.

    trunk : Linear(D_eff -> H) -> GELU -> Linear(H -> H) -> GELU
    heads : 7 x Linear(H -> 1) regressors (log10 targets)
            1 x Linear(H -> 1) failure logit
            1 x Linear(H -> A) policy (continuous dims in physical units)

which is the nine named C6 graph outputs, in ``OUTPUT_NAMES`` order. With the
production schema (D = 47, A = 9, H = 96) this is 16 177 parameters, small
enough that the dashboard's per-neuron activation view stays legible.

Activation taps
---------------
``forward_tuple_explain`` appends the trunk's own three tensors -- the
post-embedding concat and the two post-GELU hidden layers -- to the nine
contract outputs, and that is the signature the shipped graph is exported
with. They are intermediates the heads already consume rather than a second
evaluation, so a consumer that never names them pays nothing for their
existence. ``network_layout`` describes the same graph statically (widths,
labels, weight blocks) for a caller that wants to draw it.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any

import torch
from torch import Tensor, nn

if __package__ in (None, ""):  # direct `python scripts/advisor/model.py`
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    __package__ = "advisor"

from .dataset import OUTPUT_NAMES, REGRESSION_HEADS

TRUNK_LAYER_NAMES = ["trunk.fc1", "trunk.fc2"]

#: The trunk tensors appended to the C6 outputs by ``forward_tuple_explain``,
#: in that order. Named here rather than in the exporter because the network
#: decides what it can expose.
ACTIVATION_OUTPUT_NAMES = ["trunk_input", "trunk_fc1", "trunk_fc2"]


class AdvisorNet(nn.Module):
    """Multi-head advisor network. See module docstring for the layout."""

    def __init__(self, config: dict[str, Any]) -> None:
        super().__init__()
        self.config = dict(config)
        self.input_columns: list[str] = list(config["input_columns"])
        self.action_dims: list[str] = list(config["action_dims"])
        self.output_names: list[str] = list(config.get("output_names", OUTPUT_NAMES))
        self.order_column = int(config["order_column"])
        self.mesher_column = int(config["mesher_column"])
        self.hidden = int(config.get("hidden", 96))
        self.emb_dim = int(config.get("emb_dim", 4))

        n_inputs = len(self.input_columns)
        # +1 slot per table for the "unknown category" index produced by the
        # dataset imputer.
        self.n_order_slots = len(config["order_choices"]) + 1
        self.n_mesher_slots = len(config["mesher_choices"]) + 1

        continuous = [
            index for index in range(n_inputs)
            if index not in (self.order_column, self.mesher_column)
        ]
        self.register_buffer("continuous_index", torch.tensor(continuous, dtype=torch.long),
                             persistent=False)
        self.register_buffer("order_index", torch.tensor([self.order_column], dtype=torch.long),
                             persistent=False)
        self.register_buffer("mesher_index", torch.tensor([self.mesher_column], dtype=torch.long),
                             persistent=False)

        self.continuous_labels = [self.input_columns[index] for index in continuous]
        self.trunk_input_labels = (
            self.continuous_labels
            + [f"order_emb_{i}" for i in range(self.emb_dim)]
            + [f"mesher_emb_{i}" for i in range(self.emb_dim)]
        )
        self.n_trunk_inputs = len(self.trunk_input_labels)

        self.order_embedding = nn.Embedding(self.n_order_slots, self.emb_dim)
        self.mesher_embedding = nn.Embedding(self.n_mesher_slots, self.emb_dim)
        self.fc1 = nn.Linear(self.n_trunk_inputs, self.hidden)
        self.fc2 = nn.Linear(self.hidden, self.hidden)
        self.act = nn.GELU()

        self.regression_heads = nn.ModuleDict(
            {name: nn.Linear(self.hidden, 1) for name in REGRESSION_HEADS}
        )
        self.failure_head = nn.Linear(self.hidden, 1)
        self.policy_head = nn.Linear(self.hidden, len(self.action_dims))

        self.head_labels = list(REGRESSION_HEADS) + ["failure_logit"] + [
            f"policy_{name}" for name in self.action_dims
        ]

    # -- construction helpers ------------------------------------------------

    @classmethod
    def from_config(cls, config: dict[str, Any], seed: int | None = None) -> "AdvisorNet":
        if seed is not None:
            torch.manual_seed(seed)
        return cls(config)

    def n_parameters(self) -> int:
        return sum(parameter.numel() for parameter in self.parameters())

    # -- forward -------------------------------------------------------------

    def trunk_input(self, x: Tensor) -> Tensor:
        """Concatenate continuous columns with the two category embeddings."""
        continuous = torch.index_select(x, 1, self.continuous_index)
        order_raw = torch.index_select(x, 1, self.order_index)
        mesher_raw = torch.index_select(x, 1, self.mesher_index)
        order_idx = torch.clamp(torch.round(order_raw), 0.0, float(self.n_order_slots - 1))
        mesher_idx = torch.clamp(torch.round(mesher_raw), 0.0, float(self.n_mesher_slots - 1))
        order_emb = self.order_embedding(order_idx.to(torch.int64)).squeeze(1)
        mesher_emb = self.mesher_embedding(mesher_idx.to(torch.int64)).squeeze(1)
        return torch.cat([continuous, order_emb, mesher_emb], dim=1)

    def trunk(self, x: Tensor) -> tuple[Tensor, Tensor, Tensor]:
        """Return ``(trunk_input, post-GELU fc1, post-GELU fc2)``."""
        z = self.trunk_input(x)
        h1 = self.act(self.fc1(z))
        h2 = self.act(self.fc2(h1))
        return z, h1, h2

    def heads(self, h2: Tensor) -> dict[str, Tensor]:
        """Every C6 output, read off the trunk's final hidden activation.

        Split out of ``forward`` so a caller that already holds the trunk taps
        -- ``forward_tuple_explain``, ``activations`` -- never evaluates the
        trunk a second time. The exported graph therefore contains exactly one
        copy of the trunk no matter how many of its tensors are named outputs.
        """
        outputs: dict[str, Tensor] = {
            name: head(h2) for name, head in self.regression_heads.items()
        }
        outputs["failure_logit"] = self.failure_head(h2)
        outputs["policy"] = self.policy_head(h2)
        return outputs

    def forward(self, x: Tensor) -> dict[str, Tensor]:
        """Dict of C6 outputs; regressors are ``[B, 1]``, policy is ``[B, A]``."""
        return self.heads(self.trunk(x)[2])

    def forward_tuple(self, x: Tensor) -> tuple[Tensor, ...]:
        """Outputs in the exact C6 order, for ``torch.onnx.export``."""
        outputs = self.forward(x)
        return tuple(outputs[name] for name in self.output_names)

    def forward_tuple_explain(self, x: Tensor) -> tuple[Tensor, ...]:
        """``forward_tuple``, then the three taps of ACTIVATION_OUTPUT_NAMES.

        This is the signature the shipped graph is exported with. The C6
        outputs keep their names, their order and their positions, so a caller
        that asks only for those cannot tell the taps are there. The taps are
        the tensors the heads are *already* computed from, shared through
        ``heads``, so naming them adds no arithmetic -- only the option of
        fetching them.
        """
        z, h1, h2 = self.trunk(x)
        outputs = self.heads(h2)
        return tuple(outputs[name] for name in self.output_names) + (z, h1, h2)

    # -- drawable structure (C8 dump and ONNX sidecar) -----------------------

    def _structure(self) -> tuple[list[tuple[str, int, list[str]]],
                                  list[tuple[str, str, Tensor]]]:
        """The drawable graph: ``(name, size, labels)`` per layer, and one
        fully connected weight block per gap between layers.

        The single source of truth behind both views of this network -- the C8
        ``activations()`` dump the dashboard draws and the
        ``activation_layout.json`` sidecar the GUI draws -- so the two cannot
        drift apart on a layer width, a label or a weight orientation. The two
        views differ only in what they attach to this skeleton: one row of
        values, or nothing.
        """
        # The heads are separate Linear modules, so the block that draws them
        # as one layer stacks their weight rows in head_labels order.
        head_rows = [self.regression_heads[name].weight for name in REGRESSION_HEADS]
        head_rows.append(self.failure_head.weight)
        head_rows.append(self.policy_head.weight)
        head_weight = torch.cat(head_rows, dim=0)
        layers = [
            ("input", self.n_trunk_inputs, list(self.trunk_input_labels)),
            (TRUNK_LAYER_NAMES[0], self.hidden, []),
            (TRUNK_LAYER_NAMES[1], self.hidden, []),
            ("heads", len(self.head_labels), list(self.head_labels)),
        ]
        edges = [
            ("input", TRUNK_LAYER_NAMES[0], self.fc1.weight),
            (TRUNK_LAYER_NAMES[0], TRUNK_LAYER_NAMES[1], self.fc2.weight),
            (TRUNK_LAYER_NAMES[1], "heads", head_weight),
        ]
        return layers, edges

    def network_layout(self) -> dict[str, Any]:
        """The static half of ``activation_layout.json``: shape and weights.

        Hidden layers carry an explicit empty ``labels`` list rather than
        omitting the key: a drawing consumer wants to iterate labels
        unconditionally, whereas the C8 dump below is an established on-disk
        shape that omits it.
        """
        layers, edges = self._structure()
        return {
            "layers": [{"name": name, "size": size, "labels": labels}
                       for name, size, labels in layers],
            "edges": [{"from": source, "to": destination,
                       "rows": int(weight.shape[0]), "cols": int(weight.shape[1]),
                       "weights": _matrix(weight)}
                      for source, destination, weight in edges],
        }

    # -- C8 activation dump --------------------------------------------------

    @torch.no_grad()
    def activations(self, x: Tensor) -> dict[str, Any]:
        """Layer values and weight matrices for one input row (C8).

        ``x`` may be ``[D]`` or ``[1, D]``. Returns the ``layers`` / ``edges``
        halves of ``activations.json``; the caller adds ``run`` and
        ``input_case``.
        """
        if x.dim() == 1:
            x = x.unsqueeze(0)
        if x.shape[0] != 1:
            raise ValueError(f"activations() expects a single row, got {tuple(x.shape)}")
        z, h1, h2 = self.trunk(x)
        outputs = self.heads(h2)
        head_values = [float(outputs[name][0, 0]) for name in REGRESSION_HEADS]
        head_values.append(float(outputs["failure_logit"][0, 0]))
        head_values.extend(float(value) for value in outputs["policy"][0])
        values = {
            "input": [float(value) for value in z[0]],
            TRUNK_LAYER_NAMES[0]: [float(value) for value in h1[0]],
            TRUNK_LAYER_NAMES[1]: [float(value) for value in h2[0]],
            "heads": head_values,
        }

        structure, blocks = self._structure()
        layers: list[dict[str, Any]] = []
        for name, size, labels in structure:
            # Key order, and the absence of `labels` on the hidden layers, are
            # part of the shape bench/advisor/runs/*/activations.json consumers
            # already parse; do not "tidy" either one.
            layer: dict[str, Any] = {"name": name, "size": size, "values": values[name]}
            if labels:
                layer["labels"] = labels
            layers.append(layer)
        edges = [{"from": source, "to": destination, "weights": _matrix(weight)}
                 for source, destination, weight in blocks]
        return {"layers": layers, "edges": edges}


def _matrix(weight: Tensor) -> list[list[float]]:
    """``weight[j][i]`` = source neuron ``i`` -> destination neuron ``j``."""
    return weight.detach().cpu().tolist()


def main() -> int:
    import argparse

    from .dataset import load_dataset

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", default=None)
    args = parser.parse_args()

    data = load_dataset(args.csv)
    net = AdvisorNet.from_config(data.model_config(), seed=1234)
    print(net)
    print(f"D={len(data.input_columns)} A={len(data.action_dims)} "
          f"D_eff={net.n_trunk_inputs} params={net.n_parameters()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
