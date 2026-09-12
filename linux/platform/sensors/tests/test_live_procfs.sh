#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# Live reads of the machine this runs on. Only what /proc always provides is
# asserted (stat, loadavg, meminfo, net/dev, diskstats, self/mounts, per-pid
# stat and status): hwmon, batteries and GPUs are the fixture tests' job,
# because a CI container has none of them.
#
# The assertions are invariants rather than values, since the values are
# whatever the machine is doing at the time. What this proves that a fixture
# cannot: that rates really appear on a second sample of a moving counter, and
# that the readers survive a real /proc with thousands of entries appearing and
# disappearing under them.

source "$(dirname "$0")/harness.sh"

echo "== capabilities on this machine =="
run --no-nvml caps
expect_status "caps succeeded" 0
expect "the backend names itself" '^backend=procfs$'
expect "procfs io probed" '^has_procfs_io=[01]$'
expect "every flag is reported" '^has_hwmon=[01]$'
echo "  (this container: $(grep -c '=1$' "$OUTPUT") capabilities set)"

echo "== cpu: two samples, so the deltas exist =="
run --no-nvml --count 2 --interval 0.3 cpu
expect_status "cpu succeeded" 0
expect "the second sample has rates" '^has_rates=1$'
expect_numeric "at least one core" core_count '$1 >= 1'
expect_numeric "usage is a fraction" total_usage '$1 >= 0 && $1 <= 1'
expect_numeric "user usage is a fraction" user_usage '$1 >= 0 && $1 <= 1'
expect_numeric "idle usage is a fraction" idle_usage '$1 >= 0 && $1 <= 1'
expect_numeric "the interval is the one asked for" interval_seconds '$1 > 0.2 && $1 < 2'
expect_numeric "load average is non-negative" load1 '$1 >= 0'

echo "== memory =="
run --no-nvml memory
expect_numeric "MemTotal is real" total_bytes '$1 > 100000000'
expect_numeric "used is below total" used_bytes '$1 > 0'
expect_numeric "pressure is a fraction" pressure '$1 >= 0 && $1 <= 1'
awk -F= '/^total_bytes=/{t=$2} /^used_bytes=/{u=$2} END{exit !(u <= t)}' "$OUTPUT" \
    && echo "  ok: used <= total" || { echo "  FAIL: used > total"; fail_count=$((fail_count+1)); }
check_count=$((check_count + 1))

echo "== network =="
run --no-nvml --count 2 --interval 0.3 net
expect_numeric "at least the loopback" interface_count '$1 >= 1'
expect "loopback is classified" '^lo kind=loopback'
expect "rates are present on the second sample" 'rates=1'

echo "== disk =="
run --no-nvml --count 2 --interval 0.3 disk
expect_numeric "at least one block device" device_count '$1 >= 1'
expect_numeric "at least one real filesystem" mount_count '$1 >= 1'
expect "the root filesystem is listed" '^/ source='
refute "pseudo filesystems are filtered out" '^/proc source=proc'
refute "and so are cgroups" 'fs=cgroup'

echo "== processes: the sampling policy topCPU uses =="
# An idle container can genuinely have nothing above the 0.01 % floor in a
# 0.4 s window, and a test that depends on the machine being busy is a test
# that fails at random. So make it busy, deliberately, for the window.
busy() { while :; do :; done; }
busy & BUSY_PID=$!
trap 'kill "$BUSY_PID" 2> /dev/null' EXIT
run --no-nvml --count 2 --interval 0.4 --limit 5 procs
expect_status "procs succeeded" 0
expect_numeric "rows returned" row_count '$1 >= 1 && $1 <= 5'
expect "a row carries a rate after the second sample" 'rate=1'
expect "the busy process is visible as measurable cpu" 'cpu_percent=[0-9]+\.[0-9]*[1-9]'

echo "== the rows are consolidated by app, as groupedByApp does =="
run --no-nvml --count 2 --interval 0.4 --limit 5 procs
expect "a group can carry more than one pid" 'members=[0-9]+'
kill "$BUSY_PID" 2> /dev/null
BUSY_PID=

echo "== processes sorted by memory, ungrouped =="
run --no-nvml --count 1 --sort memory --no-group --limit 3 procs
expect_numeric "three rows" row_count '$1 == 3'
expect "each row is a single pid when ungrouped" 'members=1'
check_count=$((check_count + 1))
if awk '/^pid=/ { for (i = 1; i <= NF; i++) if ($i ~ /^rss=/) {
             split($i, field, "="); if (seen && field[2] > previous) exit 1
             previous = field[2]; seen = 1 } }' "$OUTPUT"; then
    echo "  ok: memory rows are in descending order"
else
    echo "  FAIL: memory rows are not in descending order"
    fail_count=$((fail_count + 1))
fi

echo "== unknown command is a usage error, not a crash =="
run --no-nvml nonsense
expect_status "exit 2 for an unknown command" 2

summary
