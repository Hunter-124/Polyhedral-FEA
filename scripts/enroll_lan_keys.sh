#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Enrol the public keys committed under docs/training/authorized-keys/ into this
# account's ~/.ssh/authorized_keys.
#
# Why a drop directory: the agents that drive training runs live on different
# machines and cannot type a password at an ssh-copy-id prompt. A public key is
# not a secret, so committing one is the one credential exchange this public
# repository can carry safely. Nothing here is automatic — a key becomes trusted
# only when a human runs this script on the target box.
#
#   ./scripts/enroll_lan_keys.sh            # enrol every new key, print what changed
#   ./scripts/enroll_lan_keys.sh --dry-run  # say what would be enrolled, write nothing
#
# Re-running is a no-op: a key already present is reported and skipped.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KEYDIR="$ROOT/docs/training/authorized-keys"
AUTH="$HOME/.ssh/authorized_keys"
DRY_RUN=0
[[ "${1:-}" == "--dry-run" ]] && DRY_RUN=1

if [[ ! -d "$KEYDIR" ]]; then
  echo "no key directory: $KEYDIR" >&2
  exit 1
fi

shopt -s nullglob
keys=("$KEYDIR"/*.pub)
if (( ${#keys[@]} == 0 )); then
  echo "no *.pub files in ${KEYDIR#"$ROOT"/}; nothing to enrol"
  exit 0
fi

mkdir -p "$HOME/.ssh"
chmod 700 "$HOME/.ssh"
touch "$AUTH"
chmod 600 "$AUTH"

added=0
for key in "${keys[@]}"; do
  name="$(basename "$key")"
  # Reject anything that is not a public key before it can reach authorized_keys.
  if ! fingerprint="$(ssh-keygen -l -f "$key" 2>/dev/null)"; then
    echo "REJECT  $name: not a valid public key" >&2
    continue
  fi
  # Compare on the key blob (field 2), not the whole line: the comment differs
  # between the committed file and an entry added earlier by ssh-copy-id.
  blob="$(awk '{print $2}' "$key")"
  if [[ -z "$blob" ]]; then
    echo "REJECT  $name: no key material" >&2
    continue
  fi
  if grep -qF -- "$blob" "$AUTH"; then
    echo "present $name  $fingerprint"
    continue
  fi
  if (( DRY_RUN )); then
    echo "would   $name  $fingerprint"
  else
    cat "$key" >> "$AUTH"
    echo "ADDED   $name  $fingerprint"
  fi
  added=$((added + 1))
done

if (( DRY_RUN )); then
  echo "dry run: $added key(s) would be added to $AUTH"
else
  echo "$added key(s) added to $AUTH"
fi
