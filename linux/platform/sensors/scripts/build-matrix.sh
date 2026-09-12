#!/bin/bash
# Build and test under every configuration this package is expected to compile
# in, because they are not the same configuration.
#
# Four of the five legs are build types. -Wformat-truncation,
# -Wmaybe-uninitialized and _FORTIFY_SOURCE all reason differently at each
# optimisation level: GCC inlines more at -O2, so it knows more about what a
# buffer can hold and warns about cases it cannot see at -O0, and vice versa. A
# tree that is clean under one build type is not thereby clean under another,
# and "no build type" is its own configuration -- that is how the WP-S1 review
# found a truncation the RelWithDebInfo build had never reported.
#
# The fifth leg is not a build type at all: an
# AddressSanitizer/UndefinedBehaviorSanitizer/LeakSanitizer build whose tests
# must pass. The four clean builds prove what the compiler can see by reading
# the code; the sanitizers prove what happens when it runs, which is a
# different question -- WP-D2 shipped a leaking CLI exit path that every one of
# the four missed. This package is nothing but string and buffer handling over
# files it did not write, so it is exactly the shape that leg is for.
#
# Configures linux/platform as a whole so a warning this package introduces in
# any other concern's build is caught here too, then runs the full ctest set.
# The sensors suites are also summarised on their own, because that is the set
# this package is answerable for.
#
# usage: sensors/scripts/build-matrix.sh [build-root]
#        (default /tmp/vorssaint-sensors-matrix)
set -u

HERE=$(cd "$(dirname "$0")/../.." && pwd)
ROOT=${1:-/tmp/vorssaint-sensors-matrix}
GEN=${GENERATOR:-Ninja}
command -v ninja > /dev/null 2>&1 || GEN="Unix Makefiles"
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
        # -DVS_SANITIZE=ON is the project's own switch (linux/platform/
        # CMakeLists.txt) rather than flags from out here, so the sanitizer
        # build stays one definition instead of two that drift. Debug, because
        # a sanitizer report is only as useful as its stack trace.
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

    if [ "$cfg" = sanitizers ]; then
        export ASAN_OPTIONS="detect_leaks=1:abort_on_error=0:strict_string_checks=1"
        export UBSAN_OPTIONS="print_stacktrace=1"
    else
        unset ASAN_OPTIONS UBSAN_OPTIONS
    fi

    # This package's own suites first, then everything, so a sensors failure is
    # never buried under another concern's missing compositor.
    if (cd "$dir" && ctest -R '^sensors' --output-on-failure > "$dir.sensors.log" 2>&1); then
        echo "  sensors ctest: $(grep -E '^[0-9]+% tests passed' "$dir.sensors.log")"
        if [ "$cfg" = sanitizers ]; then
            echo "  sanitizers: no leak, no undefined behaviour in any sensors suite"
        fi
    else
        echo "  SENSORS CTEST FAILED"
        if [ "$cfg" = sanitizers ]; then
            grep -E "ERROR: (Address|Leak)Sanitizer|runtime error:|SUMMARY:" \
                "$dir.sensors.log" | head -20 | sed 's/^/    /'
        fi
        tail -25 "$dir.sensors.log" | sed 's/^/    /'
        rc=1
    fi

    # The whole tree is *built* in every leg, which is what catches a warning
    # this package introduces elsewhere. Of the other concerns' suites, only the
    # ones that need no session stack are run, and only in one leg: the capture
    # and audio stack suites bring up sway, a portal and PipeWire, they take
    # minutes to skip when those are absent, and on a shared container a
    # half-dead stack from another agent's run can hang them outright. Their
    # result belongs to their own package's matrix, not to this one.
    if [ "$cfg" = default ]; then
        if (cd "$dir" && ctest -E 'capture_stack|audio_' --output-on-failure \
                > "$dir.ctest.log" 2>&1); then
            echo "  rest of the tree (no session stack): $(grep -E '^[0-9]+% tests passed' "$dir.ctest.log")"
        else
            echo "  rest of the tree (no session stack): $(grep -E '^[0-9]+% tests passed' "$dir.ctest.log")" \
                 "(non-sensors failures are other packages')"
            grep -E "^\s+[0-9]+ - .*(Failed|Not Run)" "$dir.ctest.log" | sed 's/^/    /'
        fi
    fi

    # The harness is where a leak would hide: every command allocates a list and
    # hands it back to a free_* member, and an exit path that skips one is
    # exactly what LeakSanitizer is here for. ctest drives it, but drive the
    # commands ctest does not (gpu, power against the live machine) too.
    if [ "$cfg" = sanitizers ]; then
        for command in caps cpu memory net disk power gpu temps procs; do
            if out=$("$dir/sensors/vs-sensors" --no-nvml --count 1 "$command" 2>&1); then
                echo "  vs-sensors $command: clean"
            elif echo "$out" | grep -qE "ERROR:|runtime error:|SUMMARY:"; then
                echo "  VS-SENSORS $command FAILED under the sanitizers"
                echo "$out" | grep -E "ERROR:|runtime error:|SUMMARY:" |
                    head -10 | sed 's/^/    /'
                rc=1
            else
                # A command that honestly cannot answer here (no battery, no
                # GPU) exits non-zero without a sanitizer finding, which is the
                # backend behaving, not a defect.
                echo "  vs-sensors $command: no data on this machine, no finding"
            fi
        done

        # A bare container exercises almost none of the allocating paths: no
        # hwmon list, no battery, no GPU, no fans. The fixture trees do, so
        # every command is driven against each of them as well -- that is where
        # a leaked temperature array or battery list would actually show up.
        fixtures="$HERE/sensors/tests/fixtures"
        for tree in amd-desktop intel-laptop thinkpad; do
            findings=0
            for command in caps cpu memory net disk power gpu temps procs; do
                out=$("$dir/sensors/vs-sensors" --root "$fixtures/$tree" --no-nvml \
                        --count 1 "$command" 2>&1 > /dev/null)
                if [ -n "$out" ]; then
                    echo "  VS-SENSORS --root $tree $command FAILED under the sanitizers"
                    echo "$out" | head -8 | sed 's/^/    /'
                    findings=1
                    rc=1
                fi
            done
            [ "$findings" -eq 0 ] && echo "  vs-sensors --root $tree: all commands clean"
        done
    fi
done

printf '\n===== summary =====\n'
if [ "$rc" -eq 0 ]; then
    echo "all five configurations build clean under -Werror and pass the sensors suites"
else
    echo "at least one configuration failed; see above"
fi
exit "$rc"
