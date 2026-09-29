# SPDX-License-Identifier: BSD-3-Clause
"""ONNX graph export of ``AdvisorNet``: the contract outputs, the trunk taps and
the ``activation_layout.json`` sidecar that describes the same network."""
from __future__ import annotations

import warnings
from pathlib import Path

import torch
from torch import Tensor, nn

from .dataset import write_json
from .model import ACTIVATION_OUTPUT_NAMES, AdvisorNet

OPSET = 17


class ExportWrapper(nn.Module):
    """Adapts ``AdvisorNet`` to the flat tuple signature ONNX needs.

    ``taps`` selects the graph shape: ``forward_tuple_explain``, whose outputs
    are the twelve contract names followed by the three trunk taps, or the bare
    ``forward_tuple`` contract. Both shapes must remain exportable: the C++
    reports ``has_activations() == false`` and still recommends against an
    untapped model directory, and ``tests/fixtures/advisor_tiny/`` is the
    fixture that pins that path.
    """

    def __init__(self, net: AdvisorNet, taps: bool) -> None:
        super().__init__()
        self.net = net
        self.taps = taps

    def forward(self, features: Tensor) -> tuple[Tensor, ...]:
        if self.taps:
            return self.net.forward_tuple_explain(features)
        return self.net.forward_tuple(features)


#: Sidecar schema id. Bump the version if a consumer could misread the payload.
ACTIVATION_LAYOUT_SCHEMA = "polymesh.advisor.activation_layout/1"

#: Sidecar file name. Lives beside ``model.onnx`` in the same model directory,
#: because it describes that exact graph's weights and would be a lie next to
#: any other one.
ACTIVATION_LAYOUT_NAME = "activation_layout.json"


def activation_layout_path(model_path: Path) -> Path:
    return model_path.with_name(ACTIVATION_LAYOUT_NAME)


def graph_output_names(net: AdvisorNet, taps: bool = True) -> list[str]:
    """Graph output order: the twelve contract names, then the activation taps.

    The contract names stay first and keep their indices because the C++ loader
    validates every output ``i`` by name before accepting the graph.
    """
    return list(net.output_names) + (list(ACTIVATION_OUTPUT_NAMES) if taps else [])


def export_graph(net: AdvisorNet, path: Path, taps: bool = True) -> Path | None:
    """Write the contract graph -- one ``features`` input, twelve named outputs --
    with the three trunk taps appended, plus its ``activation_layout.json`` sidecar.

    Naming the taps as graph outputs costs the production ``Advisor::Impl::run`
    nothing. They are not new arithmetic: ``forward_tuple_explain`` shares one
    trunk evaluation with the heads (see ``AdvisorNet.heads``), so the taps are
    tensors the graph already had to materialise on the way to the twelve
    contract outputs. ORT computes and returns only the outputs the caller names
    in ``Run``, and the C++ names exactly the twelve; the extra graph outputs
    merely stop those three intermediates from being fusible or reusable buffers.

    ``taps=False`` writes the pre-activation graph shape and no sidecar, which
    is what a model directory looked like before this feature and what the C++
    back-compat path must keep loading.

    Returns the sidecar path, or ``None`` when untapped.
    """
    net.eval()
    path.parent.mkdir(parents=True, exist_ok=True)
    dummy = torch.zeros(2, len(net.input_columns), dtype=torch.float32)
    output_names = graph_output_names(net, taps)
    dynamic_axes: dict[str, dict[int, str]] = {"features": {0: "batch"}}
    for name in output_names:
        dynamic_axes[name] = {0: "batch"}
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        torch.onnx.export(
            ExportWrapper(net, taps),
            (dummy,),
            str(path),
            input_names=["features"],
            output_names=output_names,
            dynamic_axes=dynamic_axes,
            opset_version=OPSET,
            do_constant_folding=True,
            dynamo=False,
        )
    if not taps:
        return None

    layout_path = activation_layout_path(path)
    # Written by the same call that writes the graph, never by a separate
    # command: a layout describing a different set of weights than the .onnx
    # beside it would draw a network nobody ran.
    layout = {
        "schema": ACTIVATION_LAYOUT_SCHEMA,
        "hidden": int(net.hidden),
        "activation_outputs": list(ACTIVATION_OUTPUT_NAMES),
    }
    layout.update(net.network_layout())
    write_json(layout_path, layout)
    return layout_path

