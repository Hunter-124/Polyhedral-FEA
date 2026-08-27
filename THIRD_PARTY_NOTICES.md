# Third-party notices

PolyMesh is BSD-3-Clause software. Its source and binary distributions use the following third-party components.

## Bundled or linked into distributed binaries

| Component | Version | License | Project |
|---|---:|---|---|
| Geogram PSM Delaunay and predicates | pinned revisions listed in `third_party/geogram/NOTICE` | BSD-3-Clause | https://github.com/BrunoLevy/geogram |
| Dear ImGui | 1.91.5-docking | MIT | https://github.com/ocornut/imgui |
| GLFW | 3.4 | zlib/libpng | https://www.glfw.org/ |
| ONNX Runtime CPU | 1.28.0 | MIT; upstream third-party notices also apply | https://onnxruntime.ai/ |

The binary package installs the complete license files and upstream notices for these components under its documentation directory.

## System dependencies

PolyMesh dynamically links system-provided Eigen, nlohmann/json, OpenCASCADE, OpenGL, OpenMP, and platform runtime libraries. Those libraries are not redistributed in the PolyMesh package; their own licenses and package notices apply.

Catch2 is fetched only to build and run the test suite and is not part of the runtime payload.
