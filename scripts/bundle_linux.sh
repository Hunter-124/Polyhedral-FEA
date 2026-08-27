#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
set -euo pipefail

prefix="${1:-dist/polymesh}"
stage="${2:-dist/polymesh-bundle}"

if [[ ! -d "$prefix/bin" || ! -d "$prefix/share" ]]; then
    echo "bundle_linux.sh: '$prefix' is not an installed PolyMesh prefix (bin/ and share/ are required)" >&2
    exit 1
fi
if ! command -v patchelf >/dev/null 2>&1; then
    echo "bundle_linux.sh: patchelf is required; install it (for example, apt install patchelf or dnf install patchelf)" >&2
    exit 1
fi
if ! command -v ldd >/dev/null 2>&1 || ! command -v readelf >/dev/null 2>&1; then
    echo "bundle_linux.sh: ldd and readelf are required (install your distribution's libc-bin and binutils packages)" >&2
    exit 1
fi

prefix="$(readlink -f "$prefix")"
if [[ -e "$stage" ]]; then
    stage="$(readlink -f "$stage")"
else
    stage="$(readlink -m "$stage")"
fi
if [[ "$stage" == "/" || "$stage" == "$prefix" ]]; then
    echo "bundle_linux.sh: refusing unsafe output directory '$stage'" >&2
    exit 1
fi

rm -rf "$stage"
mkdir -p "$stage/lib"
cp -a "$prefix/bin" "$stage/"
cp -a "$prefix/share" "$stage/"

# These libraries define the minimum host ABI and must match the host glibc.
allow_host_library() {
    local name="$1"
    case "$name" in
        # The ELF interpreter is part of the host glibc ABI floor.
        ld-linux*.so* | ld-linux* | */ld-linux*.so*) return 0 ;;
        # The C library defines the host ABI floor and is not safely relocatable.
        libc.so*) return 0 ;;
        # libm is shipped with glibc and must match the host C library.
        libm.so*) return 0 ;;
        # libdl is shipped with glibc and must match the host dynamic loader.
        libdl.so*) return 0 ;;
        # libpthread is shipped with glibc and must match the host threading ABI.
        libpthread.so*) return 0 ;;
        # librt is shipped with glibc and must match the host C library.
        librt.so*) return 0 ;;
        # libresolv is shipped with glibc and follows the host resolver ABI.
        libresolv.so*) return 0 ;;
        # libnsl participates in the host glibc/network-services ABI floor.
        libnsl.so*) return 0 ;;
        # libutil is shipped with glibc and must match the host C library.
        libutil.so*) return 0 ;;
        # linux-vdso is a virtual kernel-provided ELF object, not a file to bundle.
        linux-vdso.so*) return 0 ;;
        # libGL is selected by the host OpenGL driver stack.
        libGL.so*) return 0 ;;
        # libGLX is selected by the host GLVND/driver stack.
        libGLX.so*) return 0 ;;
        # libGLdispatch is the host GLVND driver dispatcher.
        libGLdispatch.so*) return 0 ;;
        # libOpenGL is selected by the host GLVND/driver stack.
        libOpenGL.so*) return 0 ;;
        # libEGL is selected by the host GPU driver stack.
        libEGL.so*) return 0 ;;
        # libGLES variants are selected by the host GPU driver stack.
        libGLESv*.so*) return 0 ;;
        # libgbm is coupled to the host Mesa/GPU driver stack.
        libgbm.so*) return 0 ;;
        # libdrm is coupled to the host kernel DRM and GPU driver stack.
        libdrm.so*) return 0 ;;
        # The X11 client libraries are pulled in by the host GLVND stack
        # (libGLX -> libX11 -> libxcb) before GLFW dlopens them, so the host copy
        # always wins the soname lookup. Bundling a second libX11 would only add
        # dead weight, and would risk two Display heaps if it ever did win.
        libX11.so* | libX11-xcb.so*) return 0 ;;
        libxcb.so* | libxcb-*.so*) return 0 ;;
        libXau.so* | libXdmcp.so*) return 0 ;;
        libXext.so* | libXrender.so* | libXfixes.so*) return 0 ;;
        libXrandr.so* | libXinerama.so* | libXcursor.so* | libXi.so*) return 0 ;;
        # Wayland/xkb clients likewise talk to the host compositor and keymaps.
        libwayland-*.so* | libxkbcommon.so* | libxkbcommon-*.so*) return 0 ;;
        *) return 1 ;;
    esac
}

declare -A origin=()
declare -A queued=()
declare -a queue=()

remember_origin() {
    local name="$1"
    local source="$2"
    if [[ -z "${origin[$name]:-}" ]]; then
        origin["$name"]="$source"
    fi
}

copy_installed_library() {
    local source="$1"
    local name="${source##*/}"
    local destination="$stage/lib/$name"
    local resolved

    if [[ -L "$source" ]]; then
        resolved="$(readlink -f "$source")"
        if [[ ! -f "$resolved" ]]; then
            echo "bundle_linux.sh: broken installed-library symlink: $source" >&2
            exit 1
        fi
        local target_name="${resolved##*/}"
        if [[ ! -e "$stage/lib/$target_name" ]]; then
            cp -aL "$resolved" "$stage/lib/$target_name"
        fi
        ln -sfn "$target_name" "$destination"
        remember_origin "$target_name" "$resolved"
    elif [[ ! -e "$destination" ]]; then
        cp -a "$source" "$destination"
    fi
    remember_origin "$name" "$source"
}

for installed_libdir in "$prefix/lib" "$prefix/lib64"; do
    if [[ -d "$installed_libdir" ]]; then
        while IFS= read -r -d '' installed_library; do
            copy_installed_library "$installed_library"
        done < <(find "$installed_libdir" \( -type f -o -type l \) \
            \( -name '*.so' -o -name '*.so.*' \) -print0)
    fi
done

enqueue_elf() {
    local candidate="$1"
    local resolved
    resolved="$(readlink -f "$candidate")"
    if [[ -z "${queued[$resolved]+present}" ]] && readelf -h "$resolved" >/dev/null 2>&1; then
        queued["$resolved"]=1
        queue+=("$resolved")
    fi
}

copy_dependency() {
    local requested_name="$1"
    local source="$2"
    local destination="$stage/lib/$requested_name"

    if [[ ! -e "$destination" ]]; then
        cp -aL "$source" "$destination"
    fi
    remember_origin "$requested_name" "$source"
    enqueue_elf "$destination"
}

mapfile -d '' binaries < <(find "$stage/bin" -maxdepth 1 -type f -perm /111 -print0)
if [[ "${#binaries[@]}" -eq 0 ]]; then
    echo "bundle_linux.sh: no executable files found in $stage/bin" >&2
    exit 1
fi
for binary in "${binaries[@]}"; do
    if readelf -h "$binary" >/dev/null 2>&1; then
        enqueue_elf "$binary"
    fi
done
while IFS= read -r -d '' installed_library; do
    enqueue_elf "$installed_library"
done < <(find "$stage/lib" -maxdepth 1 -type f \( -name '*.so' -o -name '*.so.*' \) -print0)

scan_dependencies() {
    local elf="$1"
    local output line name path
    if ! output="$(LD_LIBRARY_PATH="$stage/lib" ldd "$elf" 2>&1)"; then
        if [[ "$output" == *"statically linked"* || "$output" == *"not a dynamic executable"* ]]; then
            return
        fi
        echo "bundle_linux.sh: ldd failed for $elf" >&2
        echo "$output" >&2
        exit 1
    fi

    while IFS= read -r line; do
        if [[ "$line" == *"not found"* ]]; then
            name="${line#"${line%%[![:space:]]*}"}"
            name="${name%%[[:space:]]*}"
            echo "bundle_linux.sh: unresolved dependency '$name' while inspecting $elf" >&2
            exit 1
        elif [[ "$line" =~ ^[[:space:]]*([^[:space:]]+)[[:space:]]+\=\>[[:space:]]+(/[^[:space:]]+) ]]; then
            name="${BASH_REMATCH[1]}"
            path="${BASH_REMATCH[2]}"
        elif [[ "$line" =~ ^[[:space:]]*(/[^[:space:]]+)[[:space:]]+\( ]]; then
            path="${BASH_REMATCH[1]}"
            name="${path##*/}"
        elif [[ "$line" =~ ^[[:space:]]*([^[:space:]]+)[[:space:]]+\( ]]; then
            name="${BASH_REMATCH[1]}"
            if allow_host_library "$name"; then
                continue
            fi
            echo "bundle_linux.sh: ldd reported dependency '$name' without a filesystem path for $elf" >&2
            exit 1
        else
            continue
        fi

        if allow_host_library "$name"; then
            continue
        fi
        if [[ ! -f "$path" ]]; then
            echo "bundle_linux.sh: resolved dependency '$name' to missing file '$path'" >&2
            exit 1
        fi
        if [[ "$path" == "$stage/lib/"* ]]; then
            enqueue_elf "$path"
        else
            copy_dependency "$name" "$path"
        fi
    done <<< "$output"
}

queue_index=0
while ((queue_index < ${#queue[@]})); do
    scan_dependencies "${queue[$queue_index]}"
    ((queue_index += 1))
done

for binary in "${binaries[@]}"; do
    if readelf -h "$binary" >/dev/null 2>&1; then
        patchelf --set-rpath "\$ORIGIN/../lib" "$binary"
    fi
done
while IFS= read -r -d '' library; do
    if readelf -h "$library" >/dev/null 2>&1; then
        patchelf --set-rpath "\$ORIGIN" "$library"
    fi
done < <(find "$stage/lib" -maxdepth 1 -type f \( -name '*.so' -o -name '*.so.*' \) -print0)

package_owner() {
    local source="$1"
    local owner=""
    if command -v rpm >/dev/null 2>&1; then
        owner="$(rpm -qf --qf '%{NAME}-%{VERSION}-%{RELEASE}' "$source" 2>/dev/null || true)"
    fi
    if [[ -z "$owner" ]] && command -v dpkg >/dev/null 2>&1; then
        owner="$(dpkg -S "$source" 2>/dev/null | sed -n '1{s/: .*//;p;}' || true)"
    fi
    printf '%s' "${owner:-unowned}"
}

manifest="$stage/lib/BUNDLED-LIBRARIES.txt"
: > "$manifest"
while IFS= read -r -d '' library; do
    name="${library##*/}"
    source="${origin[$name]:-$library}"
    printf '%s\t%s\t%s\n' "$name" "$source" "$(package_owner "$source")" >> "$manifest"
done < <(find "$stage/lib" -maxdepth 1 \( -type f -o -type l \) \
    \( -name '*.so' -o -name '*.so.*' \) -print0 | sort -z)

stage_real="$(readlink -f "$stage")"
verify_binary() {
    local binary="$1"
    local output line name path resolved
    if ! output="$(env -u LD_LIBRARY_PATH ldd "$binary" 2>&1)"; then
        echo "bundle_linux.sh: verification ldd failed for $binary" >&2
        echo "$output" >&2
        exit 1
    fi
    while IFS= read -r line; do
        if [[ "$line" == *"not found"* ]]; then
            name="${line#"${line%%[![:space:]]*}"}"
            name="${name%%[[:space:]]*}"
            echo "bundle_linux.sh: verification failed: $binary cannot find $name" >&2
            exit 1
        elif [[ "$line" =~ ^[[:space:]]*([^[:space:]]+)[[:space:]]+\=\>[[:space:]]+(/[^[:space:]]+) ]]; then
            name="${BASH_REMATCH[1]}"
            path="${BASH_REMATCH[2]}"
        elif [[ "$line" =~ ^[[:space:]]*(/[^[:space:]]+)[[:space:]]+\( ]]; then
            path="${BASH_REMATCH[1]}"
            name="${path##*/}"
        elif [[ "$line" =~ ^[[:space:]]*([^[:space:]]+)[[:space:]]+\( ]]; then
            name="${BASH_REMATCH[1]}"
            if allow_host_library "$name"; then
                continue
            fi
            echo "bundle_linux.sh: verification failed: unresolved non-host dependency '$name' in $binary" >&2
            exit 1
        else
            continue
        fi

        if allow_host_library "$name"; then
            continue
        fi
        resolved="$(readlink -f "$path")"
        if [[ "$resolved" != "$stage_real/"* ]]; then
            echo "bundle_linux.sh: verification failed: $binary leaks '$name' from '$path'" >&2
            exit 1
        fi
    done <<< "$output"
}

binary_count=0
for binary in "${binaries[@]}"; do
    if readelf -h "$binary" >/dev/null 2>&1; then
        verify_binary "$binary"
        ((binary_count += 1))
    fi
done

library_count="$(wc -l < "$manifest")"
payload_size="$(du -sh "$stage" | cut -f1)"
printf 'PolyMesh bundle ready: %s binaries, %s bundled libraries, %s total\n' \
    "$binary_count" "$library_count" "$payload_size"
