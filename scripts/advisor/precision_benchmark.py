#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Measure advisor training precision on the installed GPU.

This is an experiment, not the deployed inference graph. It separates native
FP32/TF32, FP16 and BF16 training from a hybrid QAT prototype whose accuracy,
geometry, cost and feasibility branches fake-quantize weights and activations at
4/8/8/8 bits while the shared trunk runs under autocast. Fake quantization uses
a straight-through estimator; it does not claim integer backward kernels or an
INT4 training throughput that Ampere/PyTorch do not provide.
"""

from __future__ import annotations

import argparse
import json
import platform
import sys
import time
from pathlib import Path
from typing import Any

import torch
from torch import Tensor, nn
from torch.nn import functional as F

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    __package__ = "advisor"

from .dataset import (ACCURACY_HEADS, COST_HEADS, INPUT_COLUMNS, ORDER_CHOICES,
                      build_action_dims)
from .model import AdvisorNet

MESHERS = ["graded_tet", "hex", "hybrid_vem", "hybrid_zoo"]
GEOMETRY_HEADS = ["geo_chamfer", "geo_p99"]
RELATIVE_ACCURACY_HEADS = ["rel_err", "rel_err_rel"]


def model_config() -> dict[str, Any]:
    return {
        "input_columns": list(INPUT_COLUMNS),
        "action_dims": build_action_dims(MESHERS),
        "output_names": [],
        "order_column": INPUT_COLUMNS.index("order_idx"),
        "mesher_column": INPUT_COLUMNS.index("mesher_idx"),
        "order_choices": list(ORDER_CHOICES),
        "mesher_choices": list(MESHERS),
        "hidden": 96,
        "emb_dim": 4,
    }


def fake_quant_symmetric(value: Tensor, bits: int) -> Tensor:
    qmax = float((1 << (bits - 1)) - 1)
    scale = value.detach().abs().amax().div(qmax).clamp_min(torch.finfo(value.dtype).eps)
    normalized = torch.clamp(value / scale, -qmax, qmax)
    rounded = normalized + (torch.round(normalized) - normalized).detach()
    return rounded * scale


def quant_linear(layer: nn.Linear, value: Tensor, bits: int) -> Tensor:
    return F.linear(fake_quant_symmetric(value, bits),
                    fake_quant_symmetric(layer.weight, bits), layer.bias)


class HybridBranchNet(nn.Module):
    """Research-only four-branch QAT network; never substituted for model.onnx."""

    def __init__(self) -> None:
        super().__init__()
        config = model_config()
        self.shared = AdvisorNet(config)
        hidden = int(config["hidden"])
        self.branch_widths = {"accuracy": 12, "geometry": 12, "cost": 16, "feasibility": 8}
        self.branch_bits = {"accuracy": 4, "geometry": 8, "cost": 8, "feasibility": 8}
        self.branches = nn.ModuleDict(
            {name: nn.Linear(hidden, width) for name, width in self.branch_widths.items()}
        )
        head_groups = {
            **{head: "accuracy" for head in RELATIVE_ACCURACY_HEADS},
            **{head: "geometry" for head in GEOMETRY_HEADS},
            **{head: "cost" for head in COST_HEADS},
        }
        self.head_groups = head_groups
        self.heads = nn.ModuleDict(
            {head: nn.Linear(self.branch_widths[group], 1) for head, group in head_groups.items()}
        )
        self.failure = nn.Linear(self.branch_widths["feasibility"], 1)

    def forward(self, value: Tensor) -> tuple[Tensor, ...]:
        hidden = self.shared.trunk(value)[2]
        branch_values = {
            name: F.gelu(quant_linear(layer, hidden, self.branch_bits[name]))
            for name, layer in self.branches.items()
        }
        outputs = [
            quant_linear(layer, branch_values[self.head_groups[name]],
                         self.branch_bits[self.head_groups[name]])
            for name, layer in self.heads.items()
        ]
        outputs.append(quant_linear(self.failure, branch_values["feasibility"],
                                    self.branch_bits["feasibility"]))
        outputs.append(self.shared.policy_head(hidden))
        return tuple(outputs)


class SharedNet(nn.Module):
    def __init__(self) -> None:
        super().__init__()
        self.net = AdvisorNet(model_config())

    def forward(self, value: Tensor) -> tuple[Tensor, ...]:
        outputs = self.net.forward(value)
        return tuple(outputs.values())


def loss_of(outputs: tuple[Tensor, ...]) -> Tensor:
    return sum(output.float().square().mean() for output in outputs)


def run_one(kind: str, precision: str, batch: int, warmup: int, steps: int) -> dict[str, Any]:
    device = torch.device("cuda")
    torch.manual_seed(1234)
    model: nn.Module = HybridBranchNet() if kind == "hybrid_qat" else SharedNet()
    model.to(device).train()
    features = torch.randn(batch, len(INPUT_COLUMNS), device=device)
    features[:, INPUT_COLUMNS.index("order_idx")] = torch.randint(
        0, len(ORDER_CHOICES), (batch,), device=device, dtype=torch.int64).float()
    features[:, INPUT_COLUMNS.index("mesher_idx")] = torch.randint(
        0, len(MESHERS), (batch,), device=device, dtype=torch.int64).float()
    optimizer = torch.optim.AdamW(model.parameters(), lr=1e-3)
    enabled = precision in {"fp16", "bf16"}
    dtype = torch.float16 if precision == "fp16" else torch.bfloat16
    scaler = torch.amp.GradScaler("cuda", enabled=precision == "fp16")
    torch.backends.cuda.matmul.allow_tf32 = precision == "tf32"

    def step() -> None:
        optimizer.zero_grad(set_to_none=True)
        with torch.autocast("cuda", dtype=dtype, enabled=enabled):
            loss = loss_of(model(features))
        scaler.scale(loss).backward()
        scaler.step(optimizer)
        scaler.update()

    for _ in range(warmup):
        step()
    torch.cuda.synchronize()
    torch.cuda.reset_peak_memory_stats()
    started = time.perf_counter()
    for _ in range(steps):
        step()
    torch.cuda.synchronize()
    elapsed = time.perf_counter() - started
    return {
        "network": kind,
        "precision": precision,
        "batch": batch,
        "steps": steps,
        "seconds": elapsed,
        "steps_per_s": steps / elapsed,
        "examples_per_s": batch * steps / elapsed,
        "peak_memory_bytes": torch.cuda.max_memory_allocated(),
        "parameters": sum(parameter.numel() for parameter in model.parameters()),
        "native_integer_backward": False if kind == "hybrid_qat" else None,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--batch", type=int, default=4096)
    parser.add_argument("--warmup", type=int, default=20)
    parser.add_argument("--steps", type=int, default=100)
    parser.add_argument("--out", type=Path,
                        default=Path("bench/advisor/evidence/precision_benchmark.json"))
    args = parser.parse_args()
    if not torch.cuda.is_available():
        raise SystemExit("CUDA is unavailable; precision benchmark requires the training GPU")

    modes = [("shared", precision) for precision in ("fp32", "tf32", "fp16", "bf16")]
    modes += [("hybrid_qat", precision) for precision in ("fp16", "bf16")]
    rows = [run_one(network, precision, args.batch, args.warmup, args.steps)
            for network, precision in modes]
    document = {
        "schema": "polymesh.advisor.precision-benchmark/1",
        "generated_utc": __import__("datetime").datetime.now(
            __import__("datetime").timezone.utc).isoformat(),
        "host": platform.node(),
        "torch": torch.__version__,
        "cuda": torch.version.cuda,
        "gpu": torch.cuda.get_device_name(0),
        "compute_capability": list(torch.cuda.get_device_capability(0)),
        "bf16_supported": torch.cuda.is_bf16_supported(),
        "note": ("INT4/INT8 rows are QAT fake quantization with FP16/BF16 backward and "
                 "optimizer state, not native integer training."),
        "rows": rows,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    for row in rows:
        print(f"{row['network']:>10} {row['precision']:>5}: "
              f"{row['examples_per_s']:,.0f} examples/s")
    print("wrote", args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
