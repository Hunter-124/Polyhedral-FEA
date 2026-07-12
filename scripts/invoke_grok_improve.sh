#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Headless Grok improvement loop after a campaign (ADR-0022).
#
# Usage:
#   ./scripts/invoke_grok_improve.sh bench/campaigns/varyhedron-short-1
#   ./scripts/invoke_grok_improve.sh varyhedron-short-1
#
# Env:
#   GROK_MAX_TURNS   default 80
#   GROK_DRY_RUN=1   print the command only

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

ARG="${1:-}"
if [[ -z "$ARG" ]]; then
  echo "usage: $0 <campaign-name-or-dir>" >&2
  exit 2
fi

if [[ -d "$ARG" && -f "$ARG/campaign.json" ]]; then
  CAMP="$ARG"
elif [[ -d "bench/campaigns/$ARG" ]]; then
  CAMP="bench/campaigns/$ARG"
else
  echo "campaign not found: $ARG" >&2
  exit 1
fi

python3 scripts/write_grok_handoff.py "$CAMP"
HANDOFF="$CAMP/HANDOFF.md"
if [[ ! -f "$HANDOFF" ]]; then
  echo "missing $HANDOFF" >&2
  exit 1
fi

MAX_TURNS="${GROK_MAX_TURNS:-80}"
CMD=(grok -p --yolo --permission-mode bypassPermissions
  --cwd "$ROOT"
  --max-turns "$MAX_TURNS"
  --deny 'Bash(git push --force*)'
  --deny 'Bash(git push -f*)'
  --prompt-file "$HANDOFF")

echo "+ ${CMD[*]}"
if [[ "${GROK_DRY_RUN:-}" == "1" ]]; then
  exit 0
fi

if ! command -v grok >/dev/null 2>&1; then
  echo "grok CLI not found on PATH" >&2
  exit 1
fi

exec "${CMD[@]}"
