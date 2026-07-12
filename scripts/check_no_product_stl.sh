#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# CI / local gate (Lane V / V2d, ADR-0020): product code must not *write* STL.
#
# load_stl remains allowed for compare/legacy surfaces. Product fixtures are
# STEP from scripts/gen_cad_parts.py (default path; --export-stl-compare only).
# scripts/gen_part_library.py is soft-deprecated (legacy STL campaigns only).
#
# Usage (from repo root):
#   ./scripts/check_no_product_stl.sh
# Exit 0 if clean; 1 if a write-STL symbol appears under src/ or product apps.

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# Symbols that would write product STL from C++ product path.
# load_stl / parse_*stl are intentionally *not* matched.
PATTERN='write_stl|WriteSTL|save_stl|export_stl|StlWriter|RWStl::Write|StlAPI_Writer'

TARGETS=(src/geom src/mesh src/adapt src/fea src/pipeline src/bench
         apps/cli apps/gui apps/testlab)

found=0
for t in "${TARGETS[@]}"; do
  if [[ ! -d "$t" ]]; then
    continue
  fi
  if grep -RInE --include='*.cpp' --include='*.hpp' --include='*.cu' --include='*.h' \
      "$PATTERN" "$t" 2>/dev/null; then
    found=1
  fi
done

if [[ "$found" -ne 0 ]]; then
  echo "ERROR: product path must not write STL (ADR-0020 / V2d)." >&2
  echo "  Use STEP fixtures (scripts/gen_cad_parts.py). load_stl is compare/legacy only." >&2
  exit 1
fi

echo "check_no_product_stl: clean (no write-STL symbols in product src/apps)"
exit 0
