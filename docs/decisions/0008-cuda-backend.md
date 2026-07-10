# ADR-0008: CUDA backend for parallelizable kernels

- Status: accepted (2026-07-09)

## Decision
CUDA acceleration is a first-class optional backend (`POLYMESH_WITH_CUDA`),
not a late-phase afterthought:
- A dispatch layer (`fea/backend.hpp`) selects CUDA at runtime when compiled
  in and a device is present; otherwise CPU.
- Targets, in order of expected win: batched element stiffness computation,
  sparse matrix-vector products inside iterative solves (CG), error-indicator
  evaluation, and mesh-quality metrics — all embarrassingly or
  bandwidth-parallel.
- **Correctness rules**: `double` precision on GPU, no exceptions. Every CUDA
  kernel has a CPU reference twin and a parity test in `bench` (results must
  match within solver tolerance). CI builds and tests CPU-only, so a GPU is
  never required for correctness.

## Constraints acknowledged
- The dev machine's RTX 3080 Ti (GeForce/consumer) executes FP64 at 1/64 of
  FP32 rate, so FLOP-bound f64 kernels may not beat a good CPU path there;
  bandwidth-bound kernels (SpMV) and latency-hiding batch assembly still win.
  Datacenter GPUs (A100/H100 class) get the full benefit. Benchmarks in
  Tier 3 record backend used.
- CUDA toolkit is not currently installed on the dev machine; the option
  stays OFF by default until it is.

## Alternatives
- Defer all GPU work to P6 (original plan): rejected by owner — GPU
  offloading should be added wherever calculations parallelize.
- Vulkan compute / SYCL / HIP for vendor neutrality: revisit if non-NVIDIA
  targets matter; the dispatch layer keeps kernels swappable.
