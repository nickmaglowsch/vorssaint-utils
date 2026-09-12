#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# The NVML binding, three ways, on a machine with no NVIDIA driver:
#
#   1. no library at all              -> has_nvml=0, no NVIDIA row, no failure
#   2. a library that loads but whose nvmlInit refuses (the driver/library
#      mismatch every NVIDIA user has hit) -> has_nvml=0, still no failure
#   3. a library that answers          -> has_nvml=1 and the values it returned
#
# Case 3 uses the stub in nvml_stub.c, which implements the same entry points
# and ABI. That is what lets the dlopen path be tested at all here: the real
# libnvidia-ml.so.1 cannot exist in this container.

source "$(dirname "$0")/harness.sh"
ROOT="$FIXTURES/intel-laptop"

if [ ! -f "${VS_NVML_STUB:-}" ]; then
    echo "no NVML stub built at '${VS_NVML_STUB:-}'; skipping"
    exit "$SKIP_EXIT"
fi

echo "== 1. no NVML library present =="
unset VS_NVML_LIBRARY
export VS_NVML_LIBRARY=/nonexistent/libnvidia-ml.so.1
run --root "$ROOT" caps
expect_status "create succeeded without the driver" 0
expect "capability honestly clear" '^has_nvml=0$'
run --root "$ROOT" gpu
expect "only the Intel card is listed" '^gpu_count=1$'
refute "no NVIDIA row invented" 'vendor=nvidia'

echo "== 2. library present but nvmlInit refuses =="
export VS_NVML_LIBRARY="$VS_NVML_STUB"
export VS_NVML_STUB_FAIL=1
run --root "$ROOT" caps
expect_status "create succeeded" 0
expect "a failed init is not a capability" '^has_nvml=0$'
unset VS_NVML_STUB_FAIL

echo "== 3. library answers =="
export VS_NVML_LIBRARY="$VS_NVML_STUB"
run --root "$ROOT" caps
expect_status "create succeeded" 0
expect "capability set" '^has_nvml=1$'

run --root "$ROOT" gpu
expect "the NVIDIA device joins the sysfs cards" '^gpu_count=2$'
expect "named from NVML" '^nvml0 vendor=nvidia driver=nvidia name=NVIDIA GeForce RTX 4070 \(stub\)$'
expect "utilization.gpu becomes a 0...1 fraction" '^nvml0\.busy=0\.7300$'
expect "memory used" '^nvml0\.vram_used_bytes=3221225472$'
expect "memory total" '^nvml0\.vram_total_bytes=12884901888$'
expect "temperature" '^nvml0\.temperature_celsius=64\.0$'
expect "power in watts, from milliwatts" '^nvml0\.watts=148\.50$'
expect "graphics clock" '^nvml0\.clock_mhz=2610\.0$'
expect "and the Intel card is still there" '^card0 vendor=intel'

echo "== the product never links NVML =="
check_count=$((check_count + 1))
if ldd "$VS_SENSORS" 2>/dev/null | grep -q "nvidia-ml"; then
    echo "  FAIL: vs-sensors links libnvidia-ml; it must be dlopened"
    fail_count=$((fail_count + 1))
else
    echo "  ok: no NEEDED entry for libnvidia-ml (dlopen only)"
fi

summary
