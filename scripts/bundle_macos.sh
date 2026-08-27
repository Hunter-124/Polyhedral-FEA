#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Relocatable macOS bundle, the Mach-O counterpart of scripts/bundle_linux.sh.
#
# It copies the non-system dylib closure of the installed executables into
# <stage>/lib, rewrites every recorded load command to @rpath/<soname>, points
# @rpath at the bundle with @loader_path (the Mach-O spelling of $ORIGIN), and
# re-signs every file it touched. The last step is not optional: on Apple
# silicon a Mach-O whose bytes no longer match its code signature is killed at
# exec time, and install_name_tool invalidates the signature of everything it
# edits.
#
# Usage: bundle_macos.sh <install-prefix> <stage-dir>
#
# Written against the /bin/bash 3.2 that ships with macOS: no associative
# arrays, no mapfile, no `readlink -f`.
set -euo pipefail

prefix="${1:-dist/polymesh}"
stage="${2:-dist/polymesh-bundle}"

if [[ ! -d "$prefix/bin" || ! -d "$prefix/share" ]]; then
    echo "bundle_macos: '$prefix' does not look like an install prefix (no bin/ + share/)" >&2
    exit 1
fi
for tool in otool install_name_tool codesign; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "bundle_macos: $tool is required (install the Xcode command line tools)" >&2
        exit 1
    fi
done

abspath() {
    # `readlink -f` is GNU-only; BSD readlink cannot canonicalise a whole path.
    local dir base
    dir="$(dirname "$1")"
    base="$(basename "$1")"
    printf '%s/%s\n' "$(cd "$dir" && pwd -P)" "$base"
}

prefix="$(abspath "$prefix")"
if [[ -e "$stage" ]]; then
    stage_parent="$(cd "$(dirname "$stage")" && pwd -P)"
    if [[ "$stage_parent" == "/" ]]; then
        echo "bundle_macos: refusing to write a bundle directly under /" >&2
        exit 1
    fi
fi

rm -rf "$stage"
mkdir -p "$stage/lib"
cp -R "$prefix/bin" "$stage/"
cp -R "$prefix/share" "$stage/"
if [[ -d "$prefix/lib" ]]; then
    # Everything the install rules already placed in lib/ (ONNX Runtime).
    (cd "$prefix/lib" && find . -maxdepth 1 \( -type f -o -type l \) -name '*.dylib' -print0) |
        while IFS= read -r -d '' installed; do
            cp -a "$prefix/lib/${installed#./}" "$stage/lib/"
        done
fi
stage="$(abspath "$stage")"

# /usr/lib and /System are the macOS ABI floor: libSystem, libc++, libobjc and
# every framework are versioned with the OS and are not ours to redistribute.
is_system_path() {
    case "$1" in
        /usr/lib/*|/System/*) return 0 ;;
        *) return 1 ;;
    esac
}

# Newline-delimited sets, because bash 3.2 has no associative arrays.
nl='
'
copied_names="$nl"
copied_from=""
queue=()
queue_index=0

known_name() {
    case "${copied_names}" in
        *"${nl}$1${nl}"*) return 0 ;;
    esac
    return 1
}

remember_name() {
    copied_names="${copied_names}$1${nl}"
}

enqueue() {
    queue[${#queue[@]}]="$1"
}

# Every load command of a Mach-O file except the leading "file itself" line.
load_paths() {
    otool -L "$1" | tail -n +2 | sed -e 's/^[[:space:]]*//' -e 's/ (compatibility.*$//'
}

rpaths_of() {
    otool -l "$1" | awk '/^ *cmd LC_RPATH$/ {want=1; next} want && /^ *path / {print $2; want=0}'
}

resolve_special() {
    # @loader_path / @executable_path are relative to the referring binary.
    local dep="$1" owner_dir="$2"
    case "$dep" in
        @loader_path/*)     printf '%s/%s\n' "$owner_dir" "${dep#@loader_path/}" ;;
        @executable_path/*) printf '%s/%s\n' "$stage/bin" "${dep#@executable_path/}" ;;
        *)                  printf '%s\n' "$dep" ;;
    esac
}

copy_dependency() {
    local source="$1" name
    name="$(basename "$source")"
    known_name "$name" && return 0
    if [[ ! -f "$source" ]]; then
        echo "bundle_macos: dependency '$source' does not exist" >&2
        exit 1
    fi
    cp -L "$source" "$stage/lib/$name"
    chmod u+w "$stage/lib/$name"
    remember_name "$name"
    copied_from="${copied_from}${name}	${source}
"
    enqueue "$stage/lib/$name"
}

scan() {
    local file="$1" owner_dir dep resolved name
    owner_dir="$(cd "$(dirname "$file")" && pwd -P)"
    while IFS= read -r dep; do
        [[ -n "$dep" ]] || continue
        case "$dep" in
            @rpath/*)
                name="${dep#@rpath/}"
                if [[ ! -e "$stage/lib/$name" ]]; then
                    echo "bundle_macos: '$file' loads $dep but $stage/lib/$name is missing" >&2
                    exit 1
                fi
                continue
                ;;
        esac
        resolved="$(resolve_special "$dep" "$owner_dir")"
        case "$resolved" in
            "$stage"/*) continue ;;
        esac
        is_system_path "$resolved" && continue
        copy_dependency "$resolved"
    done < <(load_paths "$file")
}

binaries=()
while IFS= read -r -d '' binary; do
    [[ -x "$binary" ]] || continue
    binaries[${#binaries[@]}]="$binary"
done < <(find "$stage/bin" -maxdepth 1 -type f -print0)
if [[ "${#binaries[@]}" -eq 0 ]]; then
    echo "bundle_macos: no executables found in $stage/bin" >&2
    exit 1
fi
for binary in "${binaries[@]}"; do
    chmod u+w "$binary"
    enqueue "$binary"
done
while IFS= read -r -d '' library; do
    chmod u+w "$library"
    remember_name "$(basename "$library")"
    enqueue "$library"
done < <(find "$stage/lib" -maxdepth 1 -type f -name '*.dylib' -print0)

while ((queue_index < ${#queue[@]})); do
    scan "${queue[$queue_index]}"
    queue_index=$((queue_index + 1))
done

# --- rewrite load commands -------------------------------------------------
retarget() {
    local file="$1" dep resolved name
    while IFS= read -r dep; do
        [[ -n "$dep" ]] || continue
        case "$dep" in
            @rpath/*) continue ;;
        esac
        resolved="$(resolve_special "$dep" "$(cd "$(dirname "$file")" && pwd -P)")"
        is_system_path "$resolved" && continue
        name="$(basename "$resolved")"
        [[ -e "$stage/lib/$name" ]] || continue
        install_name_tool -change "$dep" "@rpath/$name" "$file"
    done < <(load_paths "$file")
}

set_rpath() {
    local file="$1" wanted="$2" existing
    while IFS= read -r existing; do
        [[ -n "$existing" ]] || continue
        # Homebrew Cellar paths and the $ORIGIN spelling CMake writes for UNIX
        # both mean nothing inside a relocated macOS bundle.
        install_name_tool -delete_rpath "$existing" "$file" 2>/dev/null || true
    done < <(rpaths_of "$file")
    install_name_tool -add_rpath "$wanted" "$file"
}

while IFS= read -r -d '' library; do
    install_name_tool -id "@rpath/$(basename "$library")" "$library"
    retarget "$library"
    set_rpath "$library" "@loader_path"
    codesign --force --sign - --timestamp=none "$library" >/dev/null 2>&1
done < <(find "$stage/lib" -maxdepth 1 -type f -name '*.dylib' -print0)

for binary in "${binaries[@]}"; do
    retarget "$binary"
    set_rpath "$binary" "@loader_path/../lib"
    codesign --force --sign - --timestamp=none "$binary" >/dev/null 2>&1
done

# --- provenance ------------------------------------------------------------
manifest="$stage/lib/BUNDLED-LIBRARIES.txt"
: > "$manifest"
while IFS= read -r library; do
    name="$(basename "$library")"
    source_path="$(printf '%s' "$copied_from" | awk -F'\t' -v n="$name" '$1 == n {print $2; exit}')"
    if [[ -z "$source_path" ]]; then
        source_path="$prefix/lib/$name"
    fi
    owner="-"
    case "$source_path" in
        */Cellar/*)
            # /opt/homebrew/Cellar/<formula>/<version>/lib/libfoo.dylib
            owner="$(printf '%s' "$source_path" | sed -E 's#^.*/Cellar/([^/]+)/([^/]+)/.*$#\1 \2#')"
            ;;
    esac
    printf '%s\t%s\t%s\n' "$name" "$source_path" "$owner" >> "$manifest"
done < <(find "$stage/lib" -maxdepth 1 \( -type f -o -type l \) -name '*.dylib' | sort)

# --- verify ----------------------------------------------------------------
verify_binary() {
    local file="$1" dep name ok=1
    while IFS= read -r dep; do
        [[ -n "$dep" ]] || continue
        case "$dep" in
            @rpath/*)
                name="${dep#@rpath/}"
                if [[ ! -e "$stage/lib/$name" ]]; then
                    echo "bundle_macos: $file -> $dep is not in the bundle" >&2
                    ok=0
                fi
                ;;
            @loader_path/*|@executable_path/*) ;;
            *)
                if ! is_system_path "$dep"; then
                    echo "bundle_macos: $file still loads host path $dep" >&2
                    ok=0
                fi
                ;;
        esac
    done < <(load_paths "$file")
    if ! codesign --verify --strict "$file" >/dev/null 2>&1; then
        echo "bundle_macos: $file has an invalid code signature" >&2
        ok=0
    fi
    [[ "$ok" -eq 1 ]]
}

binary_count=0
for binary in "${binaries[@]}"; do
    verify_binary "$binary"
    binary_count=$((binary_count + 1))
done
while IFS= read -r -d '' library; do
    verify_binary "$library"
done < <(find "$stage/lib" -maxdepth 1 -type f -name '*.dylib' -print0)

library_count="$(wc -l < "$manifest" | tr -d ' ')"
payload_size="$(du -sh "$stage" | cut -f1 | tr -d ' ')"
printf 'PolyMesh bundle ready: %s binaries, %s bundled libraries, %s total\n' \
    "$binary_count" "$library_count" "$payload_size"
