#!/bin/bash
# Build and test under every configuration the helper is expected to compile
# in, because they are not the same configuration.
#
# There is a fifth leg that is not a build type at all: an
# AddressSanitizer/UndefinedBehaviorSanitizer/LeakSanitizer build whose tests
# must pass. The four clean builds prove what the compiler can see by reading
# the code; the sanitizers prove what actually happens when it runs, which is a
# different question -- WP-D2 shipped a leaking CLI exit path that every one of
# the four missed.
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
# Configures linux/platform as a whole -- every concern's library, harness and
# ctest suite -- so a warning any of them introduces is caught here.
#
# usage: capture/scripts/build-matrix.sh [build-root]
#        (default /tmp/vorssaint-platform-matrix)
set -u

HERE=$(cd "$(dirname "$0")/../.." && pwd)
ROOT=${1:-/tmp/vorssaint-platform-matrix}
GEN=${GENERATOR:-Ninja}
command -v ninja >/dev/null 2>&1 || GEN="Unix Makefiles"
mkdir -p "$ROOT"

rc=0
for cfg in default Debug Release RelWithDebInfo asan; do
    dir="$ROOT/$cfg"
    rm -rf "$dir"
    printf '\n===== %s =====\n' "$cfg"

    if [ "$cfg" = default ]; then
        # Deliberately no -DCMAKE_BUILD_TYPE: this is the bare configure a
        # person types, and CMakeLists.txt must make it match what ships.
        cmake -S "$HERE" -B "$dir" -G "$GEN" > "$dir.configure.log" 2>&1
    elif [ "$cfg" = asan ]; then
        # Debug, because a sanitizer report is only as useful as its stack
        # trace. -fno-sanitize-recover makes undefined behaviour abort at the
        # point it happens instead of printing and carrying on, so a test that
        # trips it fails rather than passing with a warning nobody reads.
        cmake -S "$HERE" -B "$dir" -G "$GEN" -DCMAKE_BUILD_TYPE=Debug \
            -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g" \
            -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
            -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined" \
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

    # The stack suites bring up one shared headless session and must not be
    # run concurrently with each other; ctest's RUN_SERIAL handles that. Tests
    # that need a compositor this machine has not got return 77 and are
    # reported as "not run" rather than failing.
    # No suppression file, deliberately. GLib and PipeWire keep process-global
    # state until exit, but it stays reachable, and LeakSanitizer reports lost
    # memory rather than unfreed memory -- so there is nothing here to excuse,
    # and every report is ours. A suppression list was written and then deleted
    # once it proved to suppress nothing: carrying one would quietly cover a
    # future real leak in the same libraries.
    if [ "$cfg" = asan ]; then
        export ASAN_OPTIONS="detect_leaks=1:abort_on_error=0:detect_stack_use_after_return=1"
        export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
    else
        unset ASAN_OPTIONS LSAN_OPTIONS UBSAN_OPTIONS
    fi
    if (cd "$dir" && ctest --output-on-failure > "$dir.ctest.log" 2>&1); then
        echo "  ctest: $(grep -E '^[0-9]+% tests passed' "$dir.ctest.log")"
        skipped=$(grep -c 'Skipped' "$dir.ctest.log" || true)
        [ "${skipped:-0}" -gt 0 ] && echo "  ctest: $skipped test(s) skipped"
    else
        echo "  CTEST FAILED"
        tail -25 "$dir.ctest.log" | sed 's/^/    /'
        rc=1
    fi
    # A sanitizer report does not always fail the test that produced it -- a
    # leak is reported at exit, after the process has already decided its
    # status -- so the log is searched as well as the exit code.
    if [ "$cfg" = asan ] && grep -qE "ERROR: (Address|Leak)Sanitizer|runtime error:" \
            "$dir.ctest.log"; then
        echo "  SANITIZER REPORTS:"
        grep -E "ERROR: (Address|Leak)Sanitizer|runtime error:|SUMMARY:" \
            "$dir.ctest.log" | sort -u | head -20 | sed 's/^/    /'
        rc=1
    fi
done

printf '\n===== summary =====\n'
if [ "$rc" -eq 0 ]; then
    echo "all four build types are clean under -Werror and pass ctest, and the"
    echo "sanitizer leg reports no leak, no undefined behaviour and no bad access"
else
    echo "at least one leg failed; see above"
fi
exit "$rc"
