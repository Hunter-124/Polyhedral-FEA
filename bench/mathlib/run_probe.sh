#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Compiles and runs the GLM-vs-Eigen probe behind ADR-0044.
#
# GLM is deliberately NOT a project dependency. This script fetches a pinned GLM
# into a scratch directory, builds the probe with the same flags a Release build
# of this project uses (-O3 -DNDEBUG, no -march, no fast-math -- see the
# POLYMESH_NATIVE_ARCH / POLYMESH_ENABLE_LTO notes in the root CMakeLists.txt),
# runs it, and deletes nothing so the numbers can be re-checked.
#
#   usage: bench/mathlib/run_probe.sh [--simd] [--out FILE]
#
#   --simd      also build with -DGLM_FORCE_INTRINSICS, GLM's SIMD opt-in
#   --out FILE  tee the report to FILE as well as stdout
set -euo pipefail

GLM_VERSION="1.0.3"
GLM_SHA256="6775e47231a446fd086d660ecc18bcd076531cfedd912fbd66e576b118607001"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${POLYMESH_MATHLIB_PROBE_DIR:-${TMPDIR:-/tmp}/polymesh-mathlib-probe}"
SIMD=0
OUT=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --simd) SIMD=1; shift ;;
        --out) OUT="$2"; shift 2 ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

mkdir -p "$WORK"
GLM_ROOT="$WORK/glm-$GLM_VERSION"
# Guard on a real header, not on the directory: a partial extract leaves the
# directory in place and would otherwise be mistaken for a usable tree.
if [[ ! -f "$GLM_ROOT/glm/glm.hpp" ]]; then
    echo "==> fetching GLM $GLM_VERSION into $WORK"
    curl -fsSL -o "$WORK/glm.tar.gz" \
        "https://github.com/g-truc/glm/archive/refs/tags/$GLM_VERSION.tar.gz"
    echo "$GLM_SHA256  $WORK/glm.tar.gz" | sha256sum --check --status || {
        echo "GLM tarball checksum mismatch -- refusing to build" >&2
        exit 1
    }
    # Extract to a scratch dir and move into place, so the guard above can never
    # observe a half-written tree.
    stage="$(mktemp -d "$WORK/stage.XXXXXX")"
    tar xzf "$WORK/glm.tar.gz" -C "$stage"
    test -f "$stage/glm-$GLM_VERSION/glm/glm.hpp" || {
        echo "GLM tarball did not contain glm/glm.hpp" >&2
        rm -rf "$stage"
        exit 1
    }
    rm -rf "$GLM_ROOT"
    mv "$stage/glm-$GLM_VERSION" "$GLM_ROOT"
    rm -rf "$stage"
fi

# Eigen include path: the system package, same as find_package(Eigen3) resolves.
EIGEN_INC="$(pkg-config --cflags-only-I eigen3 2>/dev/null || echo -I/usr/include/eigen3)"

CXX="${CXX:-g++}"
# GLM_ENABLE_EXPERIMENTAL is required for glm/gtx/pca.hpp, which is where GLM's
# only eigensolver lives. That it is an experimental extension is part of the
# finding, not an accident of this script.
FLAGS=(-std=c++20 -O3 -DNDEBUG -DGLM_ENABLE_EXPERIMENTAL)

build_and_run() {
    local tag="$1"; shift
    local bin="$WORK/probe_$tag"
    echo "==> building probe ($tag) with $CXX ${FLAGS[*]} $*"
    # shellcheck disable=SC2086
    "$CXX" "${FLAGS[@]}" "$@" $EIGEN_INC -I"$GLM_ROOT" \
        "$HERE/mathlib_probe.cpp" -o "$bin"
    echo
    # Pin to one core: these are single-threaded per-op timings and migration
    # between cores is the largest noise source.
    if command -v taskset >/dev/null 2>&1; then
        taskset -c 2 "$bin"
    else
        "$bin"
    fi
}

run_all() {
    "$CXX" --version | head -1
    echo
    build_and_run default
    if [[ "$SIMD" == "1" ]]; then
        echo
        build_and_run simd -DGLM_FORCE_INTRINSICS
    fi
}

if [[ -n "$OUT" ]]; then
    mkdir -p "$(dirname "$OUT")"
    run_all | tee "$OUT"
    echo "==> report written to $OUT" >&2
else
    run_all
fi
