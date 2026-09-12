#!/bin/bash
# Build and test linux/platform under every configuration it is expected to
# compile in, because they are not the same configuration.
#
# -Wformat-truncation, -Wmaybe-uninitialized and _FORTIFY_SOURCE all reason
# differently at each optimisation level: GCC inlines more at -O2, so it knows
# more about what a buffer can hold and warns about cases it cannot see at
# -O0, and vice versa. A tree that is clean under one build type is not
# thereby clean under another, and "no build type" is its own configuration --
# that is how the WP-S1 review found a truncation the RelWithDebInfo build had
# never reported. With -Werror, a warning in any of them is a build failure
# for whoever hits it first.
#
# CTEST_ARGS narrows what is run in each configuration -- the point of the
# matrix is the compiler, and the live suites cost the same minutes four times
# over. `CTEST_ARGS="-R audio_" scripts/build-matrix.sh` is the usual form when
# the change is in one concern.
#
# usage: scripts/build-matrix.sh [build-root]      (default /tmp/vorssaint-matrix)
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=${1:-/tmp/vorssaint-matrix}
GEN=${GENERATOR:-Ninja}
command -v ninja >/dev/null 2>&1 || GEN="Unix Makefiles"
mkdir -p "$ROOT"

rc=0
for cfg in default Debug Release RelWithDebInfo; do
    dir="$ROOT/$cfg"
    rm -rf "$dir"
    printf '\n===== %s =====\n' "$cfg"

    if [ "$cfg" = default ]; then
        # Deliberately no -DCMAKE_BUILD_TYPE: this is the bare configure a
        # person types, and CMakeLists.txt must make it match what ships.
        cmake -S "$HERE" -B "$dir" -G "$GEN" > "$dir.configure.log" 2>&1
    else
        cmake -S "$HERE" -B "$dir" -G "$GEN" -DCMAKE_BUILD_TYPE="$cfg" \
            > "$dir.configure.log" 2>&1
    fi
    if [ $? -ne 0 ]; then
        echo "  CONFIGURE FAILED; see $dir.configure.log"
        tail -20 "$dir.configure.log" | sed 's/^/    /'
        rc=1
        continue
    fi
    grep -E "defaulting to" "$dir.configure.log" | sed 's/^/  /'
    echo "  CMAKE_BUILD_TYPE = $(grep -E '^CMAKE_BUILD_TYPE' "$dir/CMakeCache.txt" |
                                 cut -d= -f2)"

    if ! cmake --build "$dir" -j4 > "$dir.build.log" 2>&1; then
        echo "  BUILD FAILED"
        grep -E "error|warning" "$dir.build.log" | head -20 | sed 's/^/    /'
        rc=1
        continue
    fi
    # -Werror makes a warning a failure, but say so explicitly rather than
    # inferring it from the exit status.
    if grep -qE "warning:" "$dir.build.log"; then
        echo "  WARNINGS in a -Werror build (should be impossible):"
        grep "warning:" "$dir.build.log" | sed 's/^/    /'
        rc=1
    else
        echo "  build: clean, no compiler warnings"
    fi

    # shellcheck disable=SC2086
    if (cd "$dir" && ctest ${CTEST_ARGS:-} --output-on-failure > "$dir.ctest.log" 2>&1); then
        echo "  ctest: $(grep -E '^[0-9]+% tests passed' "$dir.ctest.log")"
    else
        echo "  CTEST FAILED"
        tail -25 "$dir.ctest.log" | sed 's/^/    /'
        rc=1
    fi
done

printf '\n===== summary =====\n'
if [ "$rc" -eq 0 ]; then
    echo "all four configurations build clean under -Werror and pass ctest"
else
    echo "at least one configuration failed; see above"
fi
exit "$rc"
