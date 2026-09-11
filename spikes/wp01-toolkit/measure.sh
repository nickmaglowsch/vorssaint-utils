#!/usr/bin/env bash
# Binary size and shared-library closure size for each candidate.
#
# The closure is every library `ldd` resolves, minus the ones an AppImage does
# not ship: glibc and its siblings (libc, libm, libpthread, libdl, librt,
# libstdc++, libgcc_s, ld-linux) and the graphics stack the host provides
# (mesa/dri/GL/EGL/GBM/drm/X11-core). What is left is what the port has to
# carry, which is the number the packaging WP cares about.
set -u

EXCLUDE='^(libc|libm|libpthread|libdl|librt|libstdc\+\+|libgcc_s|ld-linux|libGL|libEGL|libGLX|libGLdispatch|libOpenGL|libgbm|libdrm|libglapi|libX11|libxcb|libXau|libXdmcp)'

closure () {
    local bin="$1" label="$2"
    echo "### $label"
    echo "binary: $(stat -c %s "$bin") bytes  ($bin)"
    local total=0 count=0 kept=0
    while read -r name path; do
        [ -z "${path:-}" ] && continue
        [ -f "$path" ] || continue
        local sz; sz=$(stat -Lc %s "$path")
        count=$((count + 1))
        if echo "$name" | grep -qE "$EXCLUDE"; then continue; fi
        kept=$((kept + 1))
        total=$((total + sz))
    done < <(ldd "$bin" | awk '{ if ($3 ~ /^\//) print $1, $3; else if ($1 ~ /^\//) print $1, $1 }')
    echo "libs resolved by ldd: $count"
    echo "libs counted (excl. glibc/mesa/X11-core): $kept"
    echo "closure size counted: $total bytes ($((total / 1024 / 1024)) MiB)"
    echo "top 10 counted libs:"
    while read -r name path; do
        [ -z "${path:-}" ] && continue
        [ -f "$path" ] || continue
        echo "$name" | grep -qE "$EXCLUDE" && continue
        printf '%s %s\n' "$(stat -Lc %s "$path")" "$name"
    done < <(ldd "$bin" | awk '{ if ($3 ~ /^\//) print $1, $3; else if ($1 ~ /^\//) print $1, $1 }') \
      | sort -rn | head -10 | awk '{ printf "  %8.1f KiB  %s\n", $1/1024, $2 }'
    echo
}

closure "$1" "$2"
