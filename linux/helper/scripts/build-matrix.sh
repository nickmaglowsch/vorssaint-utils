#!/bin/bash
# Build and test under every configuration the helper is expected to compile
# in, because they are not the same configuration.
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
# The fifth configuration is the sanitizers, and it is here for the same
# reason: the compiler cannot see what it cannot prove. -Werror said nothing
# about vorssaint-relay leaking its whole device backend on every --replay and
# --bench run, because leaking is not a diagnosable mistake -- it is a missing
# statement. The WP-D1/D2 review found it by running this leg for the first
# time; the leg exists so the next one is found by the script instead.
#
# usage: scripts/build-matrix.sh [build-root]      (default /tmp/vorssaint-matrix)
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=${1:-/tmp/vorssaint-matrix}
GEN=${GENERATOR:-Ninja}
command -v ninja >/dev/null 2>&1 || GEN="Unix Makefiles"
mkdir -p "$ROOT"

rc=0
for cfg in default Debug Release RelWithDebInfo sanitizers; do
    dir="$ROOT/$cfg"
    rm -rf "$dir"
    printf '\n===== %s =====\n' "$cfg"

    if [ "$cfg" = default ]; then
        # Deliberately no -DCMAKE_BUILD_TYPE: this is the bare configure a
        # person types, and CMakeLists.txt must make it match what ships.
        cmake -S "$HERE" -B "$dir" -G "$GEN" > "$dir.configure.log" 2>&1
    elif [ "$cfg" = sanitizers ]; then
        # -DWITH_SANITIZERS=ON rather than flags from out here: CMakeLists.txt
        # appends -D_FORTIFY_SOURCE=2 after CMAKE_C_FLAGS, so an -D..=0 passed
        # in would only be redefined, and -Werror turns that into a failure
        # about the flags rather than about the code. The option drops fortify
        # from the inside. -O1 is what ASan documents; Debug's -O0 hides
        # inlined frames in the report.
        cmake -S "$HERE" -B "$dir" -G "$GEN" \
            -DCMAKE_BUILD_TYPE=Debug -DWITH_SANITIZERS=ON \
            -DCMAKE_C_FLAGS_DEBUG="-O1 -g" \
            > "$dir.configure.log" 2>&1
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

    # A sanitizer finding is reported on stderr and the process exits non-zero,
    # so ctest already fails on one -- but say what it was, because "test 7
    # failed" and "the relay leaks 2 MB per run" are different sentences.
    if [ "$cfg" = sanitizers ]; then
        export ASAN_OPTIONS="detect_leaks=1:abort_on_error=0:strict_string_checks=1"
        export UBSAN_OPTIONS="print_stacktrace=1"
    else
        unset ASAN_OPTIONS UBSAN_OPTIONS
    fi

    if (cd "$dir" && ctest --output-on-failure > "$dir.ctest.log" 2>&1); then
        echo "  ctest: $(grep -E '^[0-9]+% tests passed' "$dir.ctest.log")"
        if [ "$cfg" = sanitizers ]; then
            echo "  sanitizers: no leak, no undefined behaviour in any suite"
        fi
    else
        echo "  CTEST FAILED"
        if [ "$cfg" = sanitizers ]; then
            grep -E "ERROR: (Address|Leak)Sanitizer|runtime error:|SUMMARY:" \
                "$dir.ctest.log" | head -20 | sed 's/^/    /'
        fi
        tail -25 "$dir.ctest.log" | sed 's/^/    /'
        rc=1
    fi

    # The daemon and the CLI are not covered by ctest, and the CLI is where
    # the leak was. Run both under the sanitizers on their own.
    if [ "$cfg" = sanitizers ]; then
        for mode in "--bench 2000" "--tap"; do
            # shellcheck disable=SC2086
            if out=$("$dir/vorssaint-relay" --backend fake $mode 2>&1); then
                echo "  relay $mode: clean"
            else
                echo "  RELAY $mode FAILED under the sanitizers"
                echo "$out" | grep -E "ERROR:|runtime error:|SUMMARY:" |
                    head -10 | sed 's/^/    /'
                rc=1
            fi
        done
    fi
done

printf '\n===== summary =====\n'
if [ "$rc" -eq 0 ]; then
    echo "all four build types are clean under -Werror and pass ctest, and the"
    echo "fifth leg adds: no leak, no undefined behaviour, in the suites and in"
    echo "the relay CLI"
else
    echo "at least one configuration failed; see above"
fi
exit "$rc"
