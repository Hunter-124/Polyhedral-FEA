<!-- Generated 2026-08-09 by a research subagent for the variable-everything + advisor program. Raw, unedited. -->

# Training stack and C++ inference deployment

# Training in Python and shipping in C++20 without Python

## Executive answer

For the **current tabular advisor**, use **CPU LightGBM regressors trained in Python and the native LightGBM C API at inference**. Train an action-conditioned surrogate—predict error, DOF, and wall time from `(part/BC features, candidate mesh action)`—then enumerate the finite mesher choices and a fixed grid for the continuous controls. This is safer and easier to calibrate than directly emitting a mixed discrete/continuous `SimSetup`.

This adds approximately **4 MiB on Windows or 9 MiB on Linux plus the model file**, based on the uncompressed native libraries in the official LightGBM 4.7.0 wheels (3,888,128-byte DLL; 9,247,120-byte SO). The current vcpkg registry has a LightGBM 4.5.0 port; pin Python training to the same version or update both together. No Python is present in the shipped application.

Do **not** add ONNX Runtime for a tree-only first release. If a PointNet/graph model later passes held-out-part tests, ONNX Runtime CPU is then the most maintainable C++ path; budget roughly **17–25 MiB** for the full CPU runtime before the model. A reduced custom ORT build can be smaller but is a separate build-maintenance project.

---

## 1. Model families for roughly 1,000–100,000 rows

### What the evidence supports

Grinsztajn, Oyallon, and Varoquaux benchmarked 45 tabular datasets and found tree ensembles still state of the art on typical medium-sized tabular data, around 10,000 samples, even before accounting for their speed advantage. Their identified inductive biases—robustness to uninformative features, axis-aligned decision boundaries, and irregular target functions—fit engineered CAD/BC descriptors well ([NeurIPS 2022 paper](https://proceedings.neurips.cc/paper_files/paper/2022/hash/0378c7692da36807bdec87ab043cdadc-Abstract-Datasets_and_Benchmarks.html), [arXiv](https://arxiv.org/abs/2207.08815)). This is evidence for making a boosted tree the default, not evidence that every tree package wins on every PolyMesh split.

| Family | Fit for 1e3–1e5 rows | Calibration / uncertainty | Extrapolation behavior | PolyMesh judgment |
|---|---|---|---|---|
| Gradient-boosted trees | Excellent default for mixed, nonlinear tabular relationships. LightGBM/XGBoost are strongest on mostly numeric features; CatBoost is especially attractive when high-cardinality categorical fields matter. CatBoost’s ordered boosting was designed to reduce target leakage/prediction shift in categorical processing ([CatBoost paper](https://arxiv.org/abs/1706.09516)). | Raw class probabilities are not automatically calibrated. The classic comparative study found boosted-tree probabilities distorted but strong after Platt or isotonic calibration ([Niculescu-Mizil & Caruana 2005](https://www.cs.cornell.edu/~alexn/papers/calibration.icml05.crc.rev3.pdf)). For regression, use quantile objectives or, preferably, split-conformal residual calibration on held-out parts. | Trees partition at learned thresholds. Beyond all thresholds on a feature, the selected leaves stop changing; there is no learned slope outside the observed range. Boosting may produce a value outside the training response range, but it still does not extrapolate a feature trend. | **First choice.** Regularize depth/leaves and evaluate LightGBM, XGBoost, and CatBoost on group-held-out parts. Choose by held-out utility and coverage, not training loss. |
| Random forests / ExtraTrees | Very good low-tuning baseline, often less accurate than boosting but robust at the lower end of the data range. | Tree-to-tree spread is only a heuristic. Quantile Regression Forests estimate conditional quantiles and have consistency results under assumptions ([Meinshausen 2006](https://www.jmlr.org/papers/v7/meinshausen06a.html)); this is still not an OOD guarantee. Bagged trees and RFs were relatively well calibrated in the 2005 probability study, especially after calibration. | Regression RF output is an average of training responses in leaves, so it cannot escape the training-response envelope and is piecewise constant outside learned thresholds. That is conservative but not informed extrapolation. | Keep as a mandatory baseline and possibly as a disagreement/OOD signal. Do not call ensemble standard deviation a calibrated error bar. |
| Small MLP | Viable near the upper end, for smooth multi-output relationships, or when sharing an encoder with a future geometry model. It usually needs more tuning, normalization, and regularization than trees on ordinary tabular data. | Modern neural networks are often poorly calibrated; temperature scaling is a strong simple classification fix ([Guo et al. 2017](https://proceedings.mlr.press/v70/guo17a.html)). Deep ensembles help but multiply training and artifact size. | ReLU networks extrapolate affine pieces, which can become arbitrarily confident or unbounded. That is **less safe**, not more physically meaningful. Dataset-shift studies show post-hoc calibration itself degrades under shift ([Ovadia et al. 2019](https://arxiv.org/abs/1906.02530)). | Use only if grouped validation clearly beats trees or if geometry input makes it necessary. A tiny MLP is very easy to ship, but ease of inference is not a reason to accept worse decisions. |
| Gaussian process | Excellent for roughly 1e3 low-dimensional observations and Bayesian optimization/active learning. Exact GP training is cubic and stores a quadratic covariance factor, making 1e5 rows impractical without sparse/structured approximations ([Rasmussen & Williams, GPML](https://gaussianprocess.org/gpml/)). | The posterior mean/variance is principled **conditional on the kernel, likelihood, and prior being correct**. It is not distribution-free calibration. Far from data, common stationary kernels revert to their prior mean and prior variance, which is useful as an abstention signal but can still be wrong under kernel misspecification. | More honest than a naked tree/MLP when its variance rises outside support, but not “safe extrapolation.” Sparse approximations can be overconfident. | Useful in Python for experiment selection and active label generation; not the first production policy. Exact GP variance also makes a large artifact: storing an `n×n` factor is about 8 MiB at n=1,000 and about 800 MiB at n=10,000 in float64. |
| Monotone-constrained variants | LightGBM, XGBoost, and CatBoost all support monotone constraints. XGBoost documents that histogram training plus constraints can yield unnecessarily shallow trees unless `max_bin` is increased ([XGBoost docs](https://xgboost.readthedocs.io/en/stable/tutorials/monotonic.html)); LightGBM exposes basic/intermediate/advanced constraint methods and a monotone penalty ([LightGBM parameters](https://lightgbm.readthedocs.io/en/stable/Parameters.html#monotone_constraints)); CatBoost supports increasing/decreasing constraints on numerical features ([CatBoost parameters](https://catboost.ai/docs/en/references/training-parameters/common)). | Constraints enforce a shape invariant; they do not estimate uncertainty or establish coverage. They improve safety only where the asserted physics is actually global. | They prevent reversal of a constrained trend, including outside interior training regions, but tree predictions remain stepwise/flat outside the outer splits. | Apply to **action-conditioned response models**, not blindly to a direct policy. Example: at fixed part, mesher, order, and grading, predicted DOF/time should generally not increase as `mesh_size` increases. Error is less strictly monotone because mesh topology and quality can jump; impose it only after checking the campaign data and solver behavior. |

### The honest answer on “calibrated and extrapolation-safe”

No listed family is intrinsically both. The safest deployable design is a system around the model:

1. **Group splits by CAD part/family**, never random campaign rows. Rows from the same part and nearby configurations are highly correlated; random splitting creates false confidence.
2. Keep distinct training, model-selection, calibration, and final test parts.
3. Use **split conformal** intervals/sets around any chosen model. Conformal prediction gives finite-sample marginal coverage without model assumptions, but the standard theorem assumes IID/exchangeable calibration and test examples; it does not promise coverage on a new OOD CAD population ([Angelopoulos & Bates 2021](https://arxiv.org/abs/2107.07511)).
4. Record the training envelope and an OOD score (robust scaled distance, nearest-neighbor distance, or isolation forest). Outside it, abstain to the current conservative heuristic or run a small exploratory mesh campaign.
5. Have the model predict **outcomes for candidate actions**, not an unconstrained action vector. Pick the action deterministically subject to conservative upper error bounds and resource limits. This makes the objective, constraints, and fallback auditable.

### What this means for PolyMesh

Train separate scalar models for `log(error)`, `log(DOF)`, and `log(seconds)` using part/BC features plus all real `SimSetup` controls as inputs. Enumerate the ten `mesher` choices, legal integer controls, and fixed canonical grids for `mesh_size`, `element_tendency`, and `eta_target`. Rank by a documented utility such as the conformal upper error bound per predicted second/DOF. This handles mixed actions cleanly and makes monotonic constraints meaningful on selected action inputs. A direct model that emits ten unrelated fields hides feasibility and uncertainty.

---

## 2. C++ inference choices

Binary sizes below distinguish measured release binaries from planning estimates. Library authors generally do not guarantee a fixed size; compiler, link mode, operators, debug symbols, SIMD, and GPU providers matter.

| Option | Runtime dependency and C++ integration | Binary/asset weight | Windows + Linux / vcpkg | License | Assessment |
|---|---|---|---|---|---|
| **ONNX Runtime CPU** | Stable C API and header-only C++ wrapper. Load `.onnx` or `.ort`, create tensors, call `Ort::Session::Run`. Full runtime contains graph loader, optimizer, kernels, protobuf/flatbuffers support, threading, etc. Official C/C++ API: [docs](https://onnxruntime.ai/docs/api/c/). | **Measured:** official ONNX Runtime 1.28.0 Windows wheel contains a 17,766,712-byte (16.94 MiB) `onnxruntime.dll`; the Linux Python binding is a combined 25.8 MB object, so it is not a clean C-runtime comparison. Plan **15–30 MiB** full CPU runtime plus model. Official docs support operator-reduced and ORT-format minimal builds; those can be much smaller, but no universal number is promised ([custom build docs](https://onnxruntime.ai/docs/build/custom.html)). | Current vcpkg main has `onnxruntime` 1.23.2, supports static/dynamic CPU and optional CUDA/OpenVINO/TensorRT features; its manifest pulls roughly twenty packages including ONNX, protobuf, abseil, flatbuffers, re2, Eigen, and Boost ([vcpkg port](https://github.com/microsoft/vcpkg/tree/master/ports/onnxruntime)). Upstream exports `onnxruntime::onnxruntime`. Static is supported by omitting `--build_shared_lib` or selecting a static vcpkg triplet, but the transitive build is heavy ([build docs](https://onnxruntime.ai/docs/build/inferencing.html)). | MIT | Best general neural/geometry runtime and acceptable if it replaces several backends. Overkill for one small tree ensemble. |
| **Treelite / TL2cgen** | Important naming correction: Treelite 4.x is now a tree exchange/runtime library; its compiler moved to **TL2cgen**. TL2cgen imports XGBoost, LightGBM, and sklearn trees through Treelite and generates C99 or a native DLL/SO/source package. It can emit a CMake package and supports MSVC, GCC, and Clang ([Treelite docs](https://treelite.readthedocs.io/en/latest/), [TL2cgen docs](https://tl2cgen.readthedocs.io/en/latest/), [deployment](https://tl2cgen.readthedocs.io/en/latest/tutorials/deploy.html)). Generated prediction code has no Python dependency. | Model-proportional. The official deployment example emits 4,831,036 bytes of C source and a 410 KB compressed source package. A typical small ensemble should add roughly **0.1–5 MiB** of native code/data, but this is a planning estimate, not a project guarantee. Large forests create huge translation units; `parallel_comp` splits them. | MSVC/Linux supported. No current Treelite or TL2cgen port in vcpkg main; generation happens in Python and generated C is built by the application. | Apache-2.0 | Strong size/performance option after the model architecture stabilizes. Requires a Python code-generation step, generated-source review policy, and Python-vs-C golden parity tests. |
| **m2cgen** | Python transpiler for sklearn RF/ExtraTrees, LightGBM, and XGBoost into dependency-free C and other languages ([repo/support matrix](https://github.com/BayesWitnesses/m2cgen)). | Model-proportional, usually **0.1–5 MiB** for small ensembles; large ensembles produce unwieldy source. | Generated C is portable; no vcpkg runtime. However, latest PyPI release is **0.10.0 from 2022-04-26**, and its own FAQ says it only works with float64 and that target-language floating behavior can differ ([PyPI](https://pypi.org/project/m2cgen/)). | MIT | Simpler than TL2cgen, but stale and numerically less reassuring. Not recommended for load-bearing deployment without pinning and exhaustive parity vectors. |
| **lleaves** | LLVM/llvmlite compiler specifically for LightGBM. Compilation needs Python, NumPy, llvmlite; cached generated ELF/Mach-O can be linked from C and inference has no Python overhead. The project warns the generated function ABI may change between major versions ([repo](https://github.com/siboehm/lleaves)). | Generated object is model-proportional, typically **sub-MiB to a few MiB**; LLVM is only on the compilation side. | Official installation is **Linux and macOS only**. No Windows support and no vcpkg port. | MIT | Disqualified for this cross-platform requirement despite good benchmarks. |
| **LightGBM C API** | Load a native text model with `LGBM_BoosterCreateFromModelfile`, initialize `LGBM_BoosterPredictForMatSingleRowFastInit`, then use `...SingleRowFast`. C API accepts float32/float64 and has explicit cleanup/error calls ([C API](https://lightgbm.readthedocs.io/en/stable/C-API.html)). This links the full LightGBM library, including code irrelevant to inference. | **Measured from official 4.7.0 wheels:** 3,888,128-byte (3.71 MiB) Windows DLL; 9,247,120-byte (8.82 MiB) Linux SO. Plan **4–10 MiB** plus model. Static linking may change the final executable size. | Current vcpkg main has LightGBM 4.5.0; static/dynamic follows the triplet. Dependencies are Eigen, fast-double-parser, fmt, and OpenMP by default ([port](https://github.com/microsoft/vcpkg/tree/master/ports/lightgbm)). Upstream also documents `-DBUILD_STATIC_LIB=ON` ([installation guide](https://lightgbm.readthedocs.io/en/stable/Installation-Guide.html)). The port does not currently install a convenient namespaced CMake target, so use `find_path`/`find_library` as shown below. | **MIT upstream** ([license](https://github.com/lightgbm-org/LightGBM/blob/master/LICENSE)); the current vcpkg manifest labels it Apache-2.0, apparently incorrectly. Preserve the upstream MIT notice. | **Recommended now:** boring, maintained, exact native model semantics, small enough, and a working vcpkg port. |
| **XGBoost C API** | Stable C API, native model loader, and upstream `find_package(xgboost); target_link_libraries(... xgboost::xgboost)` after installing from source ([C API tutorial](https://xgboost.readthedocs.io/en/stable/tutorials/c_api_tutorial.html)). `BUILD_STATIC_LIB` exists. | **Measured from official 3.4.0 wheels:** 56,908,288-byte (54.27 MiB) Windows DLL and 86,355,977-byte (82.35 MiB) Linux SO. A tailored source build may be smaller, but plan **tens of MiB**. | Windows/Linux supported upstream. No current `ports/xgboost` entry in vcpkg main. Building and packaging from source is substantially heavier than LightGBM for an inference-only use. | Apache-2.0 | Good API, poor footprint/build trade for PolyMesh unless it wins decisively in grouped tests. |
| **CatBoost standalone C++ export** | `save_model(..., format="cpp")` emits standalone C++ with no linked CatBoost library; numerical and categorical inputs are supported, although categorical export requires the training pool ([save docs](https://catboost.ai/docs/en/concepts/python-reference_catboost_save_model), [C++ apply docs](https://catboost.ai/docs/en/concepts/c-plus-plus-api_applycatboostmodel)). Official docs say generated-code inference is slower than the native CatBoost evaluator, especially for large models/datasets. | Model-proportional, typically **0.1–5 MiB** for a small symmetric-tree ensemble. | Portable C++11/14, no vcpkg dependency. Standalone code has limitations, including documented multiclass limitations; scalar surrogate regressors avoid that issue. | Apache-2.0 | Excellent zero-runtime alternative if CatBoost wins. Prefer its official exporter over m2cgen for CatBoost models. |
| **Hand-rolled small MLP** | Export normalization, dense weights, biases, and activations to generated `constexpr` arrays; implement fixed-shape row-major GEMV/ReLU and output transforms. No general tensor runtime. | Exact float32 weight payload is `4 × Σ(n_in+1)n_out` bytes. A 64→128→64→20 network is about 69 KiB of parameters; code is normally under 20 KiB. | Native C++20/CMake, no vcpkg. | Your code/model license | Sensible for a fixed tiny MLP. “50 lines” is realistic only for dense layers and simple activations; robust shape checking, normalization, softmax, version checks, SIMD, and parity tests make it closer to 100–300 maintainable lines. Do not expand this into a home-grown PointNet/GNN runtime. |
| **Rust tract** | Real ONNX/NNEF inference engine with a C FFI, Windows/Linux support, and a translate-once/ship-reduced-NNEF workflow. Current README requires Rust 1.91 and calls only the high-level `tract` crate stable ([repo](https://github.com/sonos/tract)). | No stable published size. Budget **roughly 3–15 MiB** for a reduced CPU static library as a planning estimate; operator selection and Rust LTO dominate. | No current vcpkg port. Requires Cargo/Rust integration and C ABI ownership in an otherwise MSVC/vcpkg project. | MIT or Apache-2.0 | Technically relevant, not “irrelevant.” It may beat ORT footprint, but the second build ecosystem is not justified for this team unless ORT size becomes a hard requirement. |
| **ggml** | Low-level cross-platform tensor/graph library, not an ONNX importer. You must write model conversion, graph construction, operator mapping, and weight loading yourself ([repo](https://github.com/ggml-org/ggml)). | Core CPU library is plausibly **low single-digit MiB**; model weights are additional. Exact size is build-dependent. | Current vcpkg has `ggml` 0.11.1 and optional BLAS/CUDA/OpenCL/OpenMP/Vulkan features ([port](https://github.com/microsoft/vcpkg/tree/master/ports/ggml)). | MIT | Good primitive library for custom quantized transformer-style deployments. It saves little engineering for a PointNet/GNN and is not recommended here. |

The measured sizes come from the official package artifacts listed on [ONNX Runtime PyPI](https://pypi.org/project/onnxruntime/), [LightGBM PyPI](https://pypi.org/project/lightgbm/), and [XGBoost PyPI](https://pypi.org/project/xgboost/). They are reference measurements, not ABI promises.

### Static linking note

“Static” does not imply “tiny.” ONNX Runtime’s static target brings a large transitive dependency set; it mainly simplifies deployment into one executable. LightGBM static linking may pull substantial training/parser code because inference is not packaged as a minimal separate library. Generated tree C/C++ is the only option above whose runtime truly consists almost entirely of the model.

---

## 3. Geometry-aware PointNet/GNN inference

### PointNet-style model

The original PointNet deliberately uses a simple permutation-invariant structure over points ([PointNet paper](https://arxiv.org/abs/1612.00593)). A small variant can be expressed with standard ONNX operators:

- per-point `Gemm`/`MatMul`, bias, ReLU, optional batch normalization folded into weights;
- `ReduceMax` across points;
- concatenation with global CAD/BC features;
- a small output MLP.

For this network, **ONNX Runtime CPU is the lightest credible low-risk C++ path**, although not the smallest possible binary. It supports dynamic point counts and gives one artifact and API on MSVC/Linux. A hand-written implementation is possible but duplicates tensor layout, batching, pooling, normalization, and future operator work. `tract` is the credible smaller-runtime alternative if the team accepts Rust/Cargo.

### Message-passing GNN

A GNN normally needs gather/scatter and segment reductions. ONNX `ScatterElements` supports additive reduction from opset 16 and max/min from opset 18 ([operator specification](https://onnx.ai/onnx/operators/onnx__ScatterElements.html)). That makes many message-passing graphs representable, but representation is not the whole deployment problem:

- PyTorch Geometric/DGL exporters may introduce unsupported custom operators or data-dependent control flow;
- dynamic edge counts and scatter kernels are less consistently optimized than dense PointNet operations;
- duplicate-index reductions can have platform/thread-order floating differences;
- custom ONNX Runtime operators are possible, but then portability and maintenance worsen ([ORT custom-op docs](https://onnxruntime.ai/docs/reference/operators/add-custom-op.html)).

Prefer a **vanilla PointNet/DeepSets encoder before a general GNN**. Export at a pinned ONNX opset, run ONNX checker, and verify every exported operator against Windows and Linux CPU ORT before accepting the architecture. Avoid PointNet++ neighborhood/custom sampling ops for the first deployment unless they export cleanly.

### Is ORT the only sane option?

No:

- a fixed tiny PointNet can be hand-written using Eigen/BLAS;
- `tract` can run standard ONNX/NNEF graphs with a smaller Rust runtime;
- ncnn/OpenVINO can run many conventional networks but add conversion/platform tradeoffs;
- LibTorch is much heavier and graph-library custom ops complicate Windows deployment;
- ggml is a primitive layer, not a ready importer.

For **one small team already on CMake/vcpkg and requiring both MSVC and Linux**, ORT is the only option here that is simultaneously mainstream, model-format driven, broadly operator complete, and easy for future maintainers. It is not needed until geometry input proves worthwhile.

### Geometry determinism starts before inference

Define deterministic surface sampling: stable face ordering, fixed seed or deterministic quadrature, stable point ordering, normalized units, deterministic normal orientation, and deterministic fixture/load annotations. PointNet max pooling is permutation invariant, but preprocessing, NaNs, and equal maxima can still differ. Quantize the final sizing field and sort refinement seeds by a stable geometric key before feeding the mesher.

---

## 4. Determinism and reproducibility

### Training

A frozen artifact makes training nondeterminism irrelevant to a released executable, but retraining must still be auditable. LightGBM documents that `deterministic=true` stabilizes CPU training across thread counts with identical data/parameters, while explicitly warning that different seeds, LightGBM versions, compilers, or systems are expected to produce different results. It recommends a forced row/column strategy and notes a speed cost ([parameter docs](https://lightgbm.readthedocs.io/en/stable/Parameters.html#deterministic)). Therefore:

- train on CPU with pinned Python/package versions;
- set `deterministic=true`, `force_col_wise=true` (or a recorded `force_row_wise=true` choice), and all seeds;
- sort rows and categorical dictionaries deterministically;
- record the dataset/content hash and keep the produced artifact hash as the release truth;
- do not promise that rerunning on another OS/compiler regenerates identical bytes.

### Inference

| Backend | Same process/run | Cross-platform bit-for-bit |
|---|---|---|
| Generated/direct trees | Tree paths are deterministic for identical feature bits. Final tree-sum floating arithmetic is fixed-order in most implementations. | **Not guaranteed.** Feature preprocessing, float parsing, compiler contraction, NaN behavior, and final accumulation can differ. Generated tree code is the easiest backend to make nearly identical. |
| Hand MLP | Deterministic with fixed scalar loop order. | Not guaranteed when SIMD/BLAS/FMA implementations differ. A scalar reference can be bit-stable on controlled IEEE-754 builds, but optimized MSVC/GCC results may differ in low bits. |
| ONNX Runtime CPU | Generally repeatable for a fixed runtime/build/thread configuration. Set sequential execution and one thread. | **Not guaranteed.** Kernel selection, SIMD, Eigen/MLAS, graph optimizations, and FMA differ. `Ort::SessionOptions::SetDeterministicCompute(true)` asks for deterministic kernels where possible; it is not a cross-platform bitwise contract ([C++ API](https://onnxruntime.ai/docs/api/c/struct_ort_1_1_session_options.html)). GPU providers weaken the claim further. |
| Scatter/reduction GNN | Repeatability depends on reduction implementation and threading. | Duplicate-index sum order makes exact cross-platform agreement particularly fragile. |

### Protect deterministic meshing from floating inference

The mesher should never consume unconstrained low-bit model output directly.

1. Canonicalize input features into specified units and quantize them before inference.
2. Run advisor inference on CPU, single-threaded. Avoid GPU inference for this workload.
3. Snap outputs to a versioned policy grid: for example, `mesh_size` as an integer multiple of a declared length quantum or one of fixed ratios to bounding-box length; `element_tendency` as an integer grid; integer controls naturally canonical.
4. Apply fixed clamping, deterministic tie-breaking by the documented mesher order, and a minimum decision margin. If the best two candidates are within the uncertainty/margin threshold, select the conservative baseline.
5. Quantize `h(x)` and sort seeds by stable geometry IDs/coordinates.
6. Golden-test the **canonical `SimSetup` and seed list**, not raw floating predictions. Small floating differences are acceptable only when they collapse to the same canonical mesh inputs.

This keeps meshing bit-for-bit deterministic even when raw predictions differ slightly. A model version change is then an intentional input change, like changing a mesh setting.

---

## 5. Versioning a 1–50 MB artifact in Git

Git LFS is appropriate because the repository already depends on it. Git LFS stores a small SHA-256 pointer in Git and the object separately; GitHub’s per-file limits are 2 GB on Free/Pro, 4 GB Team, and 5 GB Enterprise Cloud, so 50 MB is technically unproblematic, although storage/download quotas still matter ([GitHub LFS overview](https://docs.github.com/en/repositories/working-with-files/managing-large-files/about-git-large-file-storage)). GitHub recommends committing `.gitattributes` ([configuration docs](https://docs.github.com/en/repositories/working-with-files/managing-large-files/configuring-git-large-file-storage)).

Recommended layout:

```text
models/mesh_advisor/
  mesh_advisor_v003.txt        # LightGBM artifact, LFS
  mesh_advisor_v003.json       # small manifest, normal Git
  parity_v003.json             # small feature/prediction/canonical-action vectors, normal Git
```

Track model formats before adding artifacts:

```bash
git lfs track "models/mesh_advisor/*.txt"
git lfs track "models/mesh_advisor/*.onnx"
git add .gitattributes
```

The normal-Git manifest should contain at least:

- policy/model semantic version and model SHA-256;
- exact feature names, order, data type, units, missing-value rule, categorical dictionaries, and feature-schema hash;
- exact output definitions/transforms and action-grid version;
- LightGBM/ONNX/exporter versions and training environment lock-file hash;
- training dataset/campaign hashes or immutable IDs, group split lists, and code commit;
- hyperparameters, seeds, monotone constraints, calibration method/quantiles;
- held-out-part metrics, conformal coverage, supported feature ranges, OOD threshold, and fallback policy;
- license/third-party notice requirements.

CI/release requirements:

- checkout with LFS enabled and run `git lfs fsck`;
- fail if the checked-out file is still an LFS pointer or its SHA-256 differs from the manifest;
- load the artifact in C++, run Python-generated parity vectors, and verify the final canonical `SimSetup`;
- never load pickle/joblib in the application or treat it as the long-term interchange format;
- use immutable versioned names; update the manifest and model in one commit; retain the prior artifact for reproducible old releases;
- avoid GitHub release URLs or network model updates at runtime—the model is part of the application release.

---

# Single recommended stack

## Training side

**Python LightGBM CPU 4.5.0**, pinned to the current vcpkg port version, with scikit-learn only for grouped splitting/calibration metrics.

- Train action-conditioned scalar regressors for `log(error)`, `log(DOF)`, and `log(seconds)`.
- Compare against RF/ExtraTrees, XGBoost, and CatBoost using group-held-out CAD parts; change the winner only on demonstrated held-out utility and calibration.
- Use conservative depth/leaves, `deterministic=true`, `force_col_wise=true`, fixed seeds, and monotone constraints only on proven action-response relationships.
- Calibrate residual intervals on separate held-out parts using split conformal.
- Save native LightGBM text models plus a normal-Git schema/metrics manifest and parity vectors.

## Inference side

**LightGBM C API, CPU, one inference thread, packaged through vcpkg.** The application enumerates a versioned candidate grid, applies the regressors, chooses deterministically using conservative intervals/resource constraints, then quantizes the chosen `SimSetup` and `h(x)` seeds.

Why this stack:

- best-supported model family for the available tabular sample range;
- no converter or generated-code semantic gap;
- Windows and Linux from one C API;
- current vcpkg port;
- much smaller than ORT/XGBoost and more maintained than m2cgen;
- expected addition **about 4 MiB Windows / 9 MiB Linux plus typically sub-MiB to a few-MiB model files**;
- leaves ONNX Runtime as a deliberate later dependency if a geometry encoder earns its cost.

## Exact vcpkg integration

Pin a vcpkg baseline in the real manifest. Disable the port’s default OpenMP feature because advisor inference is single-row/single-threaded:

```json
{
  "name": "polymesh",
  "version-string": "0.0.0",
  "builtin-baseline": "<PINNED-VCPKG-COMMIT>",
  "dependencies": [
    {
      "name": "lightgbm",
      "default-features": false
    }
  ]
}
```

Use dynamic triplets to avoid static transitive-link surprises from a library that does not export a polished CMake package target:

```powershell
# Windows, Developer PowerShell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Release
```

```bash
# Linux; x64-linux-dynamic is a current vcpkg community triplet
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=x64-linux-dynamic
cmake --build build -j
```

The current LightGBM vcpkg port installs headers and the library but no convenient exported target. Add this CMake integration:

```cmake
find_path(LIGHTGBM_INCLUDE_DIR
  NAMES LightGBM/c_api.h
  REQUIRED)
find_library(LIGHTGBM_LIBRARY
  NAMES lib_lightgbm lightgbm
  REQUIRED)

add_library(LightGBM::LightGBM INTERFACE IMPORTED)
set_property(TARGET LightGBM::LightGBM PROPERTY
  INTERFACE_INCLUDE_DIRECTORIES "${LIGHTGBM_INCLUDE_DIR}")
set_property(TARGET LightGBM::LightGBM PROPERTY
  INTERFACE_LINK_LIBRARIES "${LIGHTGBM_LIBRARY}")

target_link_libraries(polymesh PRIVATE LightGBM::LightGBM)

if(WIN32)
  find_file(LIGHTGBM_RUNTIME
    NAMES lib_lightgbm.dll
    PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin"
    NO_DEFAULT_PATH REQUIRED)
  add_custom_command(TARGET polymesh POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${LIGHTGBM_RUNTIME}" "$<TARGET_FILE_DIR:polymesh>")
  install(FILES "${LIGHTGBM_RUNTIME}" DESTINATION bin)
else()
  # LIGHTGBM_LIBRARY is the shared object under the dynamic triplet.
  set_target_properties(polymesh PROPERTIES INSTALL_RPATH "$ORIGIN")
  install(FILES "${LIGHTGBM_LIBRARY}" DESTINATION lib)
endif()
```

At runtime use, in order:

1. `LGBM_BoosterCreateFromModelfile`;
2. validate the model feature count/schema against the manifest;
3. `LGBM_BoosterPredictForMatSingleRowFastInit` with `C_API_DTYPE_FLOAT64` (or a consistently tested float32 schema) and `num_threads=1`;
4. `LGBM_BoosterPredictForMatSingleRowFast` for each candidate;
5. check every return code and report `LGBM_GetLastError`;
6. free the fast configuration and booster with the documented C API cleanup calls.

Pin Python `lightgbm==4.5.0` while the runtime port remains 4.5.0. When updating LightGBM or the vcpkg baseline, retrain/export only if intended, run Python/C++ prediction parity on boundary values, verify canonical mesh actions on Windows and Linux, and update the artifact manifest atomically.

### Conditional geometry upgrade

If a PointNet-style model later demonstrates a material held-out-part gain, add `onnxruntime` to the vcpkg manifest and link:

```cmake
find_package(onnxruntime CONFIG REQUIRED)
target_link_libraries(polymesh PRIVATE onnxruntime::onnxruntime)
```

Use CPU-only ORT, sequential execution, one thread, and a pinned ONNX opset. Expect the full CPU runtime to add roughly 17–25 MiB; only invest in a reduced ORT-format/operator build after the model and operator set are stable.
