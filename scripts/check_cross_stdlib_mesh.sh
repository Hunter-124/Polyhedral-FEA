#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# CI / local gate: the mesh must not depend on which standard library built it.
#
# Why this gate exists. On 2026-08-14 the same commit built with MSVC and with
# gcc produced different meshes on 5 of 24 corpus pairs — stepped_shaft_s2_c0
# hybrid_zoo came out 264 elements one way and 200 the other, and one pair
# flipped solve_fail -> ok. Floating point was ruled out: the gcc build was
# bit-identical across reruns and thread counts, and -ffp-contract=off changed
# nothing. The cause was iteration over std::unordered_map / std::unordered_set
# in loops whose ORDER feeds a mutation: boundary node relaxation that reverts on
# inversion, free-face lists handed to the smoother, and hp DOF numbering.
#
# libc++ hashes and buckets differently from libstdc++, so meshing the same part
# with both and comparing the written mesh byte for byte catches a reintroduced
# order dependence directly, without needing a Windows runner.
#
# Usage (from repo root), after configuring both trees:
#   cmake -S . -B build-gnu -DCMAKE_BUILD_TYPE=Release
#   cmake -S . -B build-libcxx -DCMAKE_BUILD_TYPE=Release \
#       -DCMAKE_CXX_COMPILER=clang++ -DPOLYMESH_WERROR=OFF \
#       -DCMAKE_CXX_FLAGS="-stdlib=libc++ -w" \
#       -DCMAKE_EXE_LINKER_FLAGS="-stdlib=libc++"
#   ./scripts/check_cross_stdlib_mesh.sh
#
# Exit 0 if every pair is byte-identical; 1 on the first divergence.

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

GNU_CLI="${GNU_CLI:-build-gnu/apps/cli/polymesh}"
LIBCXX_CLI="${LIBCXX_CLI:-build-libcxx/apps/cli/polymesh}"

for cli in "$GNU_CLI" "$LIBCXX_CLI"; do
    if [ ! -x "$cli" ]; then
        echo "ERROR: $cli not found or not executable."
        echo "Build both trees first (see the usage block in this script)."
        exit 1
    fi
done

# Committed fixtures only: the procedural corpus under bench/geometries/corpus is
# generated and absent from a fresh clone. icecream_cone is in the list because
# it is the part the S7 overlapped-sheet carve was written for, and the carve
# consumes exactly the free-face lists this gate protects.
CASES=(
    "tests/fixtures/parts/plate_hole.step 0.004 graded"
    "tests/fixtures/parts/plate_hole.step 0.004 hybrid"
    "tests/fixtures/parts/icecream_cone.step 0.010 graded"
    "tests/fixtures/parts/icecream_cone.step 0.010 hybrid"
)

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

status=0
for entry in "${CASES[@]}"; do
    read -r part h mesher <<<"$entry"
    name="$(basename "$part" .step)-$mesher"
    "$GNU_CLI" mesh "$part" -h "$h" --mesher "$mesher" -o "$work/$name.gnu.vtu" >/dev/null
    "$LIBCXX_CLI" mesh "$part" -h "$h" --mesher "$mesher" -o "$work/$name.libcxx.vtu" >/dev/null
    if cmp -s "$work/$name.gnu.vtu" "$work/$name.libcxx.vtu"; then
        echo "ok       $name"
    else
        echo "DIVERGED $name — libstdc++ and libc++ wrote different meshes."
        echo "         An unordered_map/unordered_set iteration order is reaching"
        echo "         node positions or cell selection again. Find the loop and"
        echo "         drive it from a sorted key vector; keep the map for lookup."
        cmp "$work/$name.gnu.vtu" "$work/$name.libcxx.vtu" || true
        status=1
    fi
done

if [ "$status" -eq 0 ]; then
    echo "cross-stdlib mesh check clean (${#CASES[@]} part/mesher pairs)"
fi
exit "$status"
