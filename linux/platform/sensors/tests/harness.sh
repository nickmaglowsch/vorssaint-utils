# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# Shared shell-test helpers. Sourced, not run.

set -u

VS_SENSORS=${1:?usage: <test>.sh /path/to/vs-sensors}
: "${VS_SOURCE_DIR:?VS_SOURCE_DIR must be set by ctest}"
FIXTURES="$VS_SOURCE_DIR/fixtures"

fail_count=0
check_count=0

# One temp file for every `run`, and exactly one EXIT trap in the whole suite.
# A trap per `run` would silently replace whatever the test had installed --
# which is how the fake-UPower suite came to leave a dbus-daemon behind on every
# invocation. A test that needs its own teardown defines `test_cleanup`.
OUTPUT=$(mktemp)
vs_at_exit() {
    if declare -f test_cleanup > /dev/null; then
        test_cleanup
    fi
    rm -f "$OUTPUT"
}
trap vs_at_exit EXIT

# ctest's "not run" code, for a dependency this machine does not have.
SKIP_EXIT=77

expect() {
    # expect <description> <pattern> ; reads the captured output from $OUTPUT
    local description=$1 pattern=$2
    check_count=$((check_count + 1))
    if grep -qE -- "$pattern" "$OUTPUT"; then
        echo "  ok: $description"
    else
        fail_count=$((fail_count + 1))
        echo "  FAIL: $description (no line matching /$pattern/)"
    fi
}

refute() {
    local description=$1 pattern=$2
    check_count=$((check_count + 1))
    if grep -qE -- "$pattern" "$OUTPUT"; then
        fail_count=$((fail_count + 1))
        echo "  FAIL: $description (unexpected line matching /$pattern/)"
    else
        echo "  ok: $description"
    fi
}

run() {
    # run <args...>; captures stdout+stderr into $OUTPUT and records the status
    echo "+ vs-sensors $*"
    "$VS_SENSORS" "$@" > "$OUTPUT" 2>&1
    RUN_STATUS=$?
    sed 's/^/    | /' "$OUTPUT"
}

expect_status() {
    local description=$1 expected=$2
    check_count=$((check_count + 1))
    if [ "$RUN_STATUS" -eq "$expected" ]; then
        echo "  ok: $description"
    else
        fail_count=$((fail_count + 1))
        echo "  FAIL: $description (exit $RUN_STATUS, wanted $expected)"
    fi
}

# Value of a `key=value` line in the captured output, or "" when absent.
value_of() {
    grep -m1 -E "^$1=" "$OUTPUT" | cut -d= -f2-
}

# expect_numeric <description> <key> <awk comparison against $1>
# e.g. expect_numeric "at least one core" core_count '$1 >= 1'
expect_numeric() {
    local description=$1 key=$2 condition=$3
    local value
    value=$(value_of "$key")
    check_count=$((check_count + 1))
    if [ -z "$value" ]; then
        fail_count=$((fail_count + 1))
        echo "  FAIL: $description (no $key= line)"
        return
    fi
    if echo "$value" | awk "{ exit !($condition) }"; then
        echo "  ok: $description ($key=$value)"
    else
        fail_count=$((fail_count + 1))
        echo "  FAIL: $description ($key=$value fails $condition)"
    fi
}

summary() {
    echo "$check_count checks, $fail_count failures"
    [ "$fail_count" -eq 0 ]
}
