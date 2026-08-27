# Install a PolyMesh release

Every release publishes one archive per platform, each with a `.sha256` sidecar:

| Platform | Archive |
|---|---|
| Linux, x86-64 | `polymesh-<version>-linux-x86_64.tar.gz` |
| Windows, x64 | `polymesh-<version>-windows-x64.zip` |
| macOS, Apple silicon | `polymesh-<version>-macos-arm64.tar.gz` |

All three are relocatable install trees: the `polymesh`, `polymesh-gui`, and `polymesh-webd`
executables, the web frontend and other runtime assets under `share/`, and the redistributable
part of the shared-library closure beside them. **None of them is a fully static build.** Each one
still loads its platform's C runtime, its graphics stack, and its GPU driver from the host; the
per-platform sections below say exactly what that means.

## Verify the download

The sidecar holds the SHA-256 of the archive followed by the path it had on the build machine, so
compare the hash field rather than running the checker against a build path:

```sh
sha256sum polymesh-<version>-linux-x86_64.tar.gz   # Linux
shasum -a 256 polymesh-<version>-macos-arm64.tar.gz # macOS
```

```powershell
(Get-FileHash -Algorithm SHA256 polymesh-<version>-windows-x64.zip).Hash.ToLower()
```

## Linux

```sh
tar -xzf polymesh-<version>-linux-x86_64.tar.gz
polymesh-<version>-linux-x86_64/bin/polymesh --version
```

### Host requirements

- A Linux system with glibc at least as new as the glibc used by the release builder.
- A working OpenGL/GLVND driver stack for `polymesh-gui`. The CLI and web server do not require a
  display server.

Extract the archive anywhere. The executables use an `$ORIGIN`-relative RUNPATH, so they can be
run directly from any working directory without setting `LD_LIBRARY_PATH` or using a launcher:

```sh
/path/to/polymesh/bin/polymesh --version
/path/to/polymesh/bin/polymesh-gui /path/to/model.step
/path/to/polymesh/bin/polymesh-webd --help
```

`lib/BUNDLED-LIBRARIES.txt` records each redistributed shared library, the builder-host path from
which it was copied, and the owning RPM or Debian package when that information was available.
It is the exact provenance inventory for that archive.

### What is not static

| Component | Delivery | Reason |
|---|---|---|
| glibc, its ELF loader, and companion libraries | Required from the host | glibc defines the host ABI floor and is not safely relocatable across Linux systems. |
| OpenGL, GLX, GLVND, EGL/GLES, GBM, DRM, and the GPU driver | Required from the host | These libraries are selected for and coupled to the installed kernel and GPU driver. |
| X11/xcb, Wayland, and xkbcommon client libraries | Required from the host | The host GLVND stack loads them before `polymesh-gui` does, so the host copy always wins the soname lookup; a second copy would only risk a split display connection. |
| `libstdc++.so.6` and `libgcc_s.so.1` | Dynamically linked but included in `lib/` | Redistributed unconditionally: the prebuilt ONNX Runtime shared library depends on both. `-DPOLYMESH_STATIC_RUNTIME=ON` (Linux CI) removes them only from the PolyMesh executables, not from the bundle. That option needs `libstdc++.a` and is `OFF` by default. |
| OpenCASCADE, ONNX Runtime, TBB, OpenMP, and the remaining runtime libraries | Dynamically linked and included in `lib/` | They are shared upstream or system builds; the bundle carries their complete non-host dependency closure and patches it to use relative RUNPATHs. |

## Windows

Extract the zip anywhere — Explorer's "Extract All", or:

```powershell
Expand-Archive polymesh-<version>-windows-x64.zip -DestinationPath C:\Tools
C:\Tools\polymesh-<version>-windows-x64\bin\polymesh.exe --version
C:\Tools\polymesh-<version>-windows-x64\bin\polymesh-gui.exe C:\models\part.step
C:\Tools\polymesh-<version>-windows-x64\bin\polymesh-webd.exe --help
```

Windows has no RUNPATH; the loader searches the directory of the running executable first, so the
bundle keeps every redistributable DLL in `bin\` next to the executables. `bin\BUNDLED-DLLS.txt`
lists them and where each came from, and `README-Windows.txt` in the archive root repeats the host
requirements below.

### Host requirements

- 64-bit Windows 10 1809 or newer.
- **Microsoft Visual C++ 2015-2022 Redistributable (x64).** These executables are compiled against
  the dynamic MSVC runtime, so `vcruntime140.dll` and `msvcp140.dll` come from the host. Most
  up-to-date machines already have it; otherwise install
  [`vc_redist.x64.exe`](https://aka.ms/vs/17/release/vc_redist.x64.exe).
- A GPU driver providing OpenGL 3.3 for `polymesh-gui.exe`. The Microsoft Basic Display Adapter
  fallback driver only offers OpenGL 1.1 and cannot run the GUI. The CLI and the web server need
  no GPU.

OpenCASCADE, ONNX Runtime, and the rest of the non-system dependency closure are inside `bin\`.
Windows' own DLLs (`kernel32`, `opengl32`, `dxgi`, the Universal CRT, and so on) are always taken
from the host.

## macOS (Apple silicon)

```sh
tar -xzf polymesh-<version>-macos-arm64.tar.gz
xattr -dr com.apple.quarantine polymesh-<version>-macos-arm64
polymesh-<version>-macos-arm64/bin/polymesh --version
```

The second line matters. The bundle is ad-hoc signed — signed so that Apple silicon will execute
it at all, but not with a Developer ID and not notarized — so Gatekeeper quarantines it after a
browser download and refuses the first launch. Clearing the quarantine attribute on the extracted
directory is the command-line equivalent of right-clicking the binary and choosing **Open**.

Mach-O binaries carry `@loader_path/../lib` in their `LC_RPATH`, the Mach-O spelling of `$ORIGIN`,
so the tree runs from anywhere without `DYLD_LIBRARY_PATH`. `lib/BUNDLED-LIBRARIES.txt` records
each bundled dylib, the path it was copied from, and its Homebrew formula and version when it came
from one.

### Host requirements

- Apple silicon (arm64) running the macOS version the release was built on or newer — currently
  macOS 14 Sonoma. There is no Intel (x86-64) archive: ONNX Runtime 1.28.0, which the mesh advisor
  links, publishes no `osx-x86_64` build.
- Nothing else to install. `libSystem`, `libc++`, `libobjc`, and the OpenGL, Cocoa, and Metal
  frameworks are part of macOS; OpenCASCADE, ONNX Runtime, and the other Homebrew-built libraries
  are inside `lib/`.

`polymesh-gui` uses the OpenGL framework, which Apple deprecated in macOS 10.14 but still ships.
