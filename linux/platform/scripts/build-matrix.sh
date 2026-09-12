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
# The fifth leg is ASan/UBSan/LSan. The four build types differ in what the
# compiler can *prove* at each optimisation level; the sanitizers see what
# actually happens at run time, which is a different class of defect -- a
# use-after-free in a callback that fires long after the call that armed it, an
# exit path that leaks (WP-D2 shipped one that four clean builds never saw).
# Neither leg substitutes for the other, so the matrix runs both.
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
for cfg in default Debug Release RelWithDebInfo Sanitizers; do
    dir="$ROOT/$cfg"
    rm -rf "$dir"
    printf '\n===== %s =====\n' "$cfg"

    if [ "$cfg" = default ]; then
        # Deliberately no -DCMAKE_BUILD_TYPE: this is the bare configure a
        # person types, and CMakeLists.txt must make it match what ships.
        cmake -S "$HERE" -B "$dir" -G "$GEN" > "$dir.configure.log" 2>&1
    elif [ "$cfg" = Sanitizers ]; then
        # Debug underneath: the sanitizers want frame pointers and unoptimised
        # code to name the frame that allocated, and an optimising build can
        # fold away the very access being checked.
        cmake -S "$HERE" -B "$dir" -G "$GEN" -DCMAKE_BUILD_TYPE=Debug \
            -DVS_SANITIZE=ON > "$dir.configure.log" 2>&1
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
    if [ "$cfg" = Sanitizers ]; then
        # Say it out loud rather than trusting the option: a leg that silently
        # built without the sanitizers would be four clean builds wearing a
        # fifth name.
        if grep -q 'fsanitize=address' "$dir/CMakeCache.txt" ||
           grep -rq 'fsanitize=address' "$dir"/*/CMakeFiles/*/flags.make 2>/dev/null ||
           grep -rq 'fsanitize=address' "$dir"/build.ninja 2>/dev/null; then
            echo "  sanitizers: address, undefined, leak"
        else
            echo "  SANITIZERS NOT ENABLED in the sanitizer leg"
            rc=1
            continue
        fi
    fi

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

    # UBSan is built with -fno-sanitize-recover, so undefined behaviour aborts
    # the test rather than printing and carrying on. detect_leaks is on by
    # default with ASan on Linux, and is named here so a future change to that
    # default cannot quietly drop the leg WP-D2 needed.
    # shellcheck disable=SC2086
    if (cd "$dir" \
        && ASAN_OPTIONS="detect_leaks=1:abort_on_error=0:${ASAN_OPTIONS:-}" \
           UBSAN_OPTIONS="print_stacktrace=1:${UBSAN_OPTIONS:-}" \
           ctest ${CTEST_ARGS:-} --output-on-failure > "$dir.ctest.log" 2>&1); then
        echo "  ctest: $(grep -E '^[0-9]+% tests passed' "$dir.ctest.log")"
    else
        echo "  CTEST FAILED"
        grep -E "AddressSanitizer|LeakSanitizer|runtime error" "$dir.ctest.log" |
            head -10 | sed 's/^/    /'
        tail -25 "$dir.ctest.log" | sed 's/^/    /'
        rc=1
    fi
    # A sanitizer report that did not fail a test is still a finding: ctest
    # only sees the exit status, and a leak reported by a process that exited 0
    # (a test that forks, say) would otherwise pass unnoticed.
    if grep -qE "AddressSanitizer:|LeakSanitizer:|runtime error:" "$dir.ctest.log"; then
        echo "  SANITIZER REPORTS in a passing run:"
        grep -E "AddressSanitizer:|LeakSanitizer:|runtime error:" "$dir.ctest.log" |
            head -10 | sed 's/^/    /'
        rc=1
    fi
done

printf '\n===== summary =====\n'
if [ "$rc" -eq 0 ]; then
    echo "all five legs pass: four build types clean under -Werror, plus"
    echo "ASan/UBSan/LSan with no sanitizer reports"
else
    echo "at least one leg failed; see above"
fi
exit "$rc"
