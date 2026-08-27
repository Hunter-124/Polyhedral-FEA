# Third-party notices

PolyMesh is BSD-3-Clause software. Its source and binary distributions use the third-party
components listed below. Exact library filenames and builder-package provenance for a release
tarball are recorded in that archive's `lib/BUNDLED-LIBRARIES.txt`.

## Components included in source or linked into PolyMesh

| Component | Version | License | Project |
|---|---:|---|---|
| Geogram PSM Delaunay and predicates | pinned revisions listed in `third_party/geogram/NOTICE` | BSD-3-Clause | https://github.com/BrunoLevy/geogram |
| Eigen | build-system selected | MPL-2.0 | https://eigen.tuxfamily.org/ |
| nlohmann/json | 3.11 or later | MIT | https://github.com/nlohmann/json |
| Dear ImGui | 1.91.5-docking | MIT | https://github.com/ocornut/imgui |
| GLFW | 3.4 | Zlib | https://www.glfw.org/ |
| ONNX Runtime CPU | 1.28.0 | MIT; upstream third-party notices also apply | https://onnxruntime.ai/ |
| Rubik | vendored font files | OFL-1.1 | https://github.com/googlefonts/rubik |
| JetBrains Mono | vendored font file | OFL-1.1 | https://www.jetbrains.com/lp/mono/ |

The installed documentation directory contains the complete licenses and upstream notices shipped
with the vendored code. `share/polymesh/fonts/OFL.txt` contains the SIL Open Font License 1.1 for
both font families.

## Distribution modes

A source or ordinary system build resolves OpenCASCADE, TBB, OpenMP, font rendering, and platform
libraries from the build host. Packaging and licensing of those dynamically linked libraries is
then the responsibility of that system.

The official relocatable Linux tarball instead copies the non-host shared-library closure into its
`lib/` directory. The closure is computed from the actual release executables and libraries, so its
exact filenames vary with the builder distribution. The following components comprise that
redistributed closure:

| Bundled component or family | License |
|---|---|
| Open CASCADE Technology (OCCT) | LGPL-2.1-only with the OCCT additional exception |
| oneAPI Threading Building Blocks (oneTBB) | Apache-2.0 |
| GNU OpenMP runtime (`libgomp`) | GPL-3.0-or-later with GCC Runtime Library Exception 3.1 |
| GNU C++ runtime (`libstdc++`) | GPL-3.0-or-later with GCC Runtime Library Exception 3.1 |
| GCC low-level runtime (`libgcc_s`) | GPL-3.0-or-later with GCC Runtime Library Exception 3.1 |
| FreeType | FTL or GPL-2.0-only |
| Fontconfig | MIT-style Fontconfig license |
| Expat | MIT |
| HarfBuzz | MIT |
| Graphite2 | LGPL-2.1-or-later or MPL-2.0 |
| libpng | Libpng |
| FreeImage | GPL-2.0-only, GPL-3.0-only, or FreeImage Public License 1.0 |
| Imath | BSD-3-Clause |
| libjpeg/libjpeg-turbo | IJG, BSD-3-Clause, and Zlib portions |
| jxrlib | BSD-2-Clause |
| OpenEXR | BSD-3-Clause |
| OpenJPEG | BSD-2-Clause |
| LibRaw | LGPL-2.1-only or CDDL-1.0 |
| libtiff | LibTIFF |
| libwebp | BSD-3-Clause |
| libdeflate | MIT |
| Little CMS | MIT |
| LERC | Apache-2.0 |
| JasPer | JasPer-2.0 |
| JBIG-KIT | GPL-2.0-or-later |
| Zstandard | BSD-3-Clause or GPL-2.0-only |
| Brotli | MIT |
| libxml2 | MIT |
| ICU | Unicode-3.0 and ICU |
| zlib | Zlib |
| bzip2 | bzip2-1.0.6 |
| XZ Utils (`liblzma`) | 0BSD |
| GLib | LGPL-2.1-or-later |
| PCRE2 | BSD-3-Clause |
| GLU (when selected by the builder's OCCT packages) | SGI-B-2.0 |
| X11 client libraries (`libX11`, `libXext`, `libXrandr`, `libXinerama`, `libXcursor`, `libXi`, `libXrender`, and `libXfixes`) | MIT/X11 |
| XCB and X authentication/transport libraries (`libxcb`, `libXau`, and `libXdmcp`) | MIT/X11 |
| libbsd | BSD-2-Clause, BSD-3-Clause, BSD-4-Clause, ISC, and MIT portions |
| libmd | BSD-2-Clause, BSD-3-Clause, BSD-4-Clause, ISC, and Beerware portions |

The Linux bundle intentionally does not copy glibc, its ELF loader and companion libraries, or the
OpenGL/GLX/GLVND/EGL/GLES/GBM/DRM stack. glibc defines the host ABI floor; graphics libraries and
drivers must match the host kernel and GPU. These are the bundle's documented host requirements,
not bundled components.

Catch2 is fetched only when building the test suite and is not part of the runtime payload.
