# `linux/platform/sensors`: the sensors backend

The Linux side of everything the system monitor, the fan-control policy and the
battery features read: CPU, memory, network, disk, power, GPU, temperatures,
fans and per-process usage. One C library (`vs_sensors`), one CLI harness
(`vs-sensors`), and the `sensors` section of
[`../include/vorssaint_platform.h`](../include/vorssaint_platform.h).

It supplies **raw samples and nothing else**. Formatting, history buffers,
sampling cadence and alert hysteresis are pure Swift that already exists in
`Sources/VorssaintCore/Services/Metrics` (`MetricFormat`,
`MonitorSamplingPolicy`, `SustainedAlertGate`, `BatteryTimeSupport`,
`PeripheralBatterySupport`, `DiskSupport`, `SpeedTest`) and is reused unchanged.
Nothing here returns a string a user reads, a percentage rounded for display, or
a decision about when to sample.

```
sensors/
  vs_sensors.c          the vtable, capability probing, dispatch-only events
  util.c                rooted paths and the small file readers
  cpu.c memory.c net.c disk.c        /proc
  hwmon.c                            /sys/class/hwmon + the classification rules
  power_sysfs.c power_upower.c       /sys/class/power_supply, UPower over sd-bus
  gpu.c nvml.c                       amdgpu/i915 sysfs, NVML via dlopen
  procs.c                            /proc/<pid> + .desktop name resolution
  tools/vs_sensors_cli.c             the `vs-sensors` harness
  tests/                             ctest suites, fixtures and fakes
  scripts/build-matrix.sh            the five-leg build
```

## Everything is rooted

Every reader takes its paths through `vs_path()`, which prefixes
`vs_sensors_options.root`. Passing a directory there replays a captured machine:

```sh
vs-sensors --root tests/fixtures/amd-desktop temps
```

That is not a testing convenience bolted on afterwards; it is how this package
is developed at all, because the container it is written in has no hwmon, no
battery, no GPU and no PSI. The one call that cannot be redirected is `statvfs`
on a mount point, and under a non-`/` root it is applied to the rooted path, so
it still measures a real filesystem — the mount tests assert the parse and the
filtering, never the size.

`--root` also forces the sysfs power backend, because a fixture tree has no bus.

## The API

`vs_sensors_system_create()` probes and returns the vtable. It fails only when
`/proc/stat` cannot be read under the root: a missing sensor is a capability
bit, never a missing backend.

| member | answers | empty when |
|---|---|---|
| `cpu` | aggregate + per-core usage, load, topology, frequency | never (rates absent on the first call) |
| `memory` | the panel's memory rows and pressure | never |
| `network` / `free_network` | per-interface counters, rates, kind, default route | never |
| `disk_devices` / `disk_mounts` | throughput per device, capacity per mount | never |
| `temperatures` / `fans` | hwmon readings, classified | no `has_hwmon` |
| `power` | battery + adapter | `VS_ERR_UNSUPPORTED` without UPower and without power_supply |
| `peripheral_batteries` | mouse, keyboard, headset | empty list |
| `gpus` | per-card telemetry | empty list |
| `processes` | top-N by CPU or memory | never |
| `thermal_pressure` | the `ProcessInfo.ThermalState` replacement | `VS_ERR_UNSUPPORTED` without hwmon |

Conventions, all of them the ones `../README.md` lays down for every section:

- **Per-call ownership.** Every list is released by its matching `free_*`. The
  single-value calls fill a caller-owned struct and allocate nothing.
- **Rates are per instance.** `cpu`, `network`, `disk_devices` and `processes`
  subtract this instance's previous call. The first call of an instance has
  `has_rates`/`has_rate` false and zeroed rates, which is exactly what the macOS
  side does by returning `nil` until it has a previous tick sample.
- **Events come only from `dispatch`.** `event_fd` is -1: hwmon has no
  notification interface, and a UPower signal loop would need a thread this
  layer refuses to start. `dispatch` compares the state the last read computed
  and calls the callback on the caller's own thread.
- **Capabilities only shrink, on an event.** The only channel that can go away
  is UPower; when it does, `dispatch` clears `VS_SENSORS_HAS_UPOWER`, repoints
  `power_backend_name`, and announces `VS_SENSORS_EVENT_BACKEND_LOST`.
- **Read-back where a request can be refused.** Nothing here writes, so there is
  nothing to read back — with one exception in spirit: a fan's
  `is_controllable` is a statement about a `pwm<N>` file this layer actually
  saw, with its path, so the helper (WP-C6) is later asked about a real file
  rather than a guess. **Nothing in this library ever writes to sysfs.**
- **"Unknown" is never zero.** `min_rpm`, `max_rpm` and `pwm` are -1 when
  absent; every optional scalar has its own `has_*` flag rather than a sentinel.

## How WP-12's `SystemSensors` mirrors it

`Sources/VorssaintCore/Platform/SystemSensors.swift` is the Swift protocol;
`Sources/VorssaintLinux` wraps this table and adds no behaviour.

| C | Swift |
|---|---|
| `vs_sensors_system` | `protocol SystemSensors` |
| `capabilities` (`vs_sensors_capability`) | the `PlatformCapability` values at the foot of `SystemSensors.swift` (`sensorsCPU`, `sensorsMemoryPressure`, `sensorsFanSpeed`, …) |
| `vs_cpu_sample` | `CPUSample` (`total_usage` → `totalUsage`, `cores[].usage` → `perCoreUsage`) |
| `vs_memory_sample` | `MemorySample` (see the mapping table below) |
| `vs_temperature_sample` | `TemperatureSample` (`id`, `label`, `celsius`, `is_primary`) |
| `vs_fan_sample` | `FanSample` (`min_rpm`/`max_rpm` of -1 become `nil`) |
| `vs_battery_sample` | `BatterySample`; `state` splits into `isCharging`, and `has_*` flags become optionals |
| `vs_power_sample` | the parts of `PowerReading` the panel keeps, plus `BatterySample` |
| `vs_network_sample` | `NetworkSample` |
| `vs_disk_device_sample` + `vs_disk_mount_sample` | `DiskSample`, joined on `device` |
| `vs_process_sample` | `ProcessUsage` (WP-A1's panel work) |
| `vs_thermal_pressure` | `ThermalPressure` |
| `set_event_callback` + `dispatch` | `observeThermalPressure` / `stopObserving` |
| `vs_result` | `throws`, `VS_OK` mapped to a normal return |

Two open points for the Swift side, both flagged to the lead rather than
decided here:

1. `MemorySample.activeBytes`'s doc comment says it is
   `MemTotal - MemAvailable` on Linux. That quantity is "memory in use", which
   is `usedBytes`; `activeBytes` is macOS's **app memory** and its Linux
   counterpart is `AnonPages`. This layer publishes both (`used_bytes` and
   `app_bytes`) under the names that say what they are, and the comment in
   `SystemSensors.swift` needs the one-line correction.
2. `MemorySample.pressure` is `Double?`. Linux supplies a number either way but
   the two are different quantities, so `pressure_is_psi` has to reach the UI:
   with PSI it is a stall fraction, without it a fill level. A `Bool` alongside
   the value, or the `sensorsMemoryPressure` capability, can carry that.

## Exact mapping, macOS metric → Linux source

### CPU (`SystemMonitor.readCPUUsage`, `host_statistics(HOST_CPU_LOAD_INFO)`)

| macOS | Linux |
|---|---|
| `cpu_ticks` user / system / idle / nice, delta over the interval | `/proc/stat` `cpu` line, same subtraction |
| busy = user + system + nice | busy = user + nice + system + irq + softirq + steal — irq/softirq/steal have no Mach bucket and are work, so they go to system |
| — | `iowait` is reported separately and is not busy |
| per-core (`processor_info`) | the `cpuN` lines |
| `activeProcessorCount` | count of `cpuN` lines |
| — | `/proc/loadavg`, which macOS's panel does not show but the Linux one can |
| — | topology from `/sys/devices/system/cpu/cpu*/topology/{physical_package_id,core_id}`: distinct `(package, core)` pairs are the physical cores |
| — | frequency from `cpufreq/scaling_cur_freq`, else the `cpu MHz` line of `/proc/cpuinfo` (a VM or an ACPI-only laptop has no cpufreq tree) |
| returns `nil` with no previous sample | `has_rates` false, rates zeroed |

### Memory (`MetricFormat.memoryUsed` / `appMemory` / `cachedFiles`)

This is the mapping the work package asked to have written down.

| macOS row | macOS source | Linux source | why |
|---|---|---|---|
| **Memory Used** | app + wired + compressed + tagged storage | `MemTotal - MemAvailable` | Activity Monitor's Memory Used is "what the machine cannot hand to a new allocation without reclaiming"; `MemAvailable` is the kernel's own estimate of exactly that, and like Memory Used it excludes reclaimable file cache while including unreclaimable kernel memory |
| **App Memory** | `internal_page_count - purgeable_count` | `AnonPages` | both are the anonymous memory of processes: not file-backed, not droppable |
| **Cached Files** | `external_page_count` | `Buffers + Cached + SReclaimable - Shmem` | file-backed pages the kernel can drop. `Shmem` is subtracted because tmpfs and shared anonymous memory are not droppable, and `SReclaimable` is added because the dentry/inode caches are |
| **Compressed** | `compressor_page_count` | `Zswapped`, when the kernel has zswap | macOS always compresses; Linux usually does not. Without zswap the field is absent (`has_compressed` false) and the row is hidden, rather than shown as a permanent zero |
| **Swap Used** | `xsw_usage.xsu_used` | `SwapTotal - SwapFree` | same quantity |
| **Memory Pressure** | `kern.memorystatus_vm_pressure_level` (normal/warning/critical) | `/proc/pressure/memory` `some avg10 / 100` | **the closest analogue.** Both answer "is the machine hurting for memory *right now*", not "how full is it": PSI's `some avg10` is the share of the last ten seconds in which at least one task stalled on memory reclaim, which is the same event macOS raises the pressure level for. `pressure_is_psi` says when this is the real thing |
| | | fallback: `1 - MemAvailable/MemTotal` | a fill level, not a stall. Reported with `pressure_is_psi` false so the UI can label it honestly; kernels without `CONFIG_PSI` (and cgroup-v1-only systems) land here |
| — | — | `MemAvailable` is also published raw | the mapping above is built on it and the panel's tooltip explains it |

Total is `MemTotal`. Pre-3.14 kernels with no `MemAvailable` fall back to the
documented approximation `MemFree + Buffers + Cached + SReclaimable`.

### Network (`NetworkSampler`, `MetricFormat.netSpeed`)

| macOS | Linux |
|---|---|
| `getifaddrs` + `if_data` byte counters | `/proc/net/dev` columns 1 and 9 |
| speed = positive delta / elapsed, 0 on a counter reset | `vs_rate()`, the same rule, tested against it |
| `MetricFormat.includeNetworkInterface`'s prefix blocklist (`lo`, `utun`, `bridge`, `awdl`, …) | `vs_network_kind`: `loopback` from `type == ARPHRD_LOOPBACK`, `wifi` from the `phy80211` symlink (or the legacy `wireless` directory), `ethernet` when a `device` symlink exists, `virtual` when it does not. The panel excludes loopback and virtual for the same reason macOS excludes tunnels: a VPN must not double-count the physical NIC |
| the primary interface from `SCNetworkReachability` | the interface named by the `0.0.0.0/0` row of `/proc/net/route`, or the `::/0` row of `/proc/net/ipv6_route` |
| — | errors, drops and packet counts, which the kernel gives for free |
| session totals accumulated in `NetworkSampler` | unchanged: this layer hands over cumulative counters and rates, and `NetworkSampler`'s accumulation logic is reused |

`NetworkCounterFallback` (the macOS bug where inbound counters freeze while
outbound keeps moving) has no Linux counterpart and is not ported; the Swift
side simply never activates it on this platform.

### Disk (`DiskSampler`, `MetricFormat.diskSpeed`)

| macOS | Linux |
|---|---|
| `IOBlockStorageDriver` `Statistics` bytes read/written | `/proc/diskstats` fields 6 and 10 × 512. The kernel documents these sector counts as fixed 512-byte units regardless of the device's logical block size |
| rate = positive delta / elapsed | `vs_rate()` |
| DiskArbitration volume list | `/proc/self/mounts`, minus a blocklist of pseudo filesystems (proc, sysfs, cgroup, tmpfs, devpts, overlay, fuse control, …) |
| `URLResourceValues` capacity/available | `statvfs`: `f_blocks × f_frsize` and `f_bavail × f_frsize` (the unprivileged free figure, which is what a file manager shows) |
| `isInternal` / `isRemovable` / `isEjectable` | not answered here. UDisks2 owns removability and eject, and that is WP-A1's panel work, not a sysfs read |
| **SMART / NVMe health** (`DiskSMARTReading`) | **out of scope, deliberately.** On macOS these come from IOKit properties that need no privilege. On Linux, SMART needs `SG_IO` on the block device and NVMe health needs `NVME_IOCTL_ADMIN_CMD`; both are root-only on a stock distribution. That makes them a `vorssaint-helper` method with its own polkit action (WP-S1, `docs/linux-port/PRIVILEGES.md`), not something an unprivileged library can read. The fields stay `nil`, the panel hides the rows, and **nothing here opens a block device** |

### Power (`PowerSampler`, `PowerReading`)

UPower first when it is on the system bus, `/sys/class/power_supply` otherwise.
The sysfs path is fully implemented, not a stub, and is the one the fixture
tests drive.

| `PowerReading` | UPower | `/sys/class/power_supply` |
|---|---|---|
| `chargePercent` | `Percentage` | `capacity`, else `energy_now / energy_full` |
| `isCharging` / state | `State` | `status` (`Not charging` becomes pending-charge, which is what a firmware charge threshold looks like) |
| `externalConnected` | a `Type=LinePower` device with `Online` | a `Mains`/`USB`/`USB_PD` supply with `online == 1` |
| `batteryWatts` | `EnergyRate`, signed by `State` | `power_now`, else `voltage_now × current_now`, signed by `status`. UPower's rate is unsigned, exactly as AppleSmartBattery's `Amperage` is signed — the sign convention is preserved on both paths |
| `timeRemainingSeconds` | `TimeToEmpty` | `time_to_empty_now`, else `energy_now / power_now` |
| (charging estimate) | `TimeToFull` | `time_to_full_now`, else the missing energy over the charge rate |
| `healthPercent` | `Capacity` (a percentage of design → 0…1 here) | `energy_full / energy_full_design`, or the `charge_*` pair |
| `cycleCount` | `ChargeCycles` (UPower 0.99.12+; absent before, and then the field stays absent rather than being guessed) | `cycle_count` |
| battery temperature | `Temperature` | `temp`, in deci-degrees |
| `adapterWatts` | — UPower publishes none, so sysfs is read even on the UPower path | the mains supply's `power_now`, else `voltage_now × current_now` |
| `adapterMaxWatts` | — | `voltage_max_design × current_max` (a USB-PD charger's rating), else `input_power_limit` |
| `systemWatts` (SMC `PSTR`) | — | **no Linux equivalent.** While discharging, the battery's own draw *is* the machine's draw and is reported as `system_watts`; while plugged in nothing on a stock kernel measures it, and the field stays absent rather than being synthesised. RAPL measures the package, not the machine, and is not a substitute |
| peripheral batteries (`PeripheralBatterySupport`) | every device with `PowerSupply == false`: mouse, keyboard, headset, typed from UPower's `Type` | a battery whose `scope` is `Device`, typed from its model name. HID peripherals that bind a `power_supply` node show up without UPower this way |

A battery that reports charge in µAh rather than energy in µWh (most phones,
some ThinkPads) is converted with `voltage_now`; health is computed from the
`charge_*` pair directly when it is, since both sides are in the same unit and
the voltage would cancel.

### Temperatures and fans (`TemperatureSensorSelector`, SMC)

macOS decides which sensor is "the CPU" from a per-chip-generation table of SMC
keys, with the hottest plausible reading as the fallback for a Mac that does not
carry its family's sensors. Linux has no key space; it has driver names, and
they split in two:

**Single-purpose drivers — the name is the answer.**

| driver | kind |
|---|---|
| `coretemp`, `k10temp`, `k8temp`, `zenpower`, `cpu_thermal`, `cpu-thermal`, `via-cputemp` | CPU |
| `amdgpu`, `radeon`, `nouveau`, `nvidia`, `i915`, `xe` | GPU |
| `nvme`, `drivetemp` | drive |
| `acpitz`, `pch_*` | system (ambient/chipset) |
| `BAT*`, `bq27*`, `rt5033-battery`, `surface_battery` | battery |

**Label-driven chips — the name says nothing.** `thinkpad`, `dell_smm`,
`asus*`, `nct*`, `it87`, `f71*`, `w836*`, `applesmc` publish a dozen unrelated
sensors under one name, so the per-sensor `temp*_label` decides, by the same
keywords a person would read: `GPU`/`graphics` → GPU; `CPU`/`Core`/`Package`/
`Tctl`/`Tdie`/`Tccd` → CPU; `Batt`/`Bat0` → battery; `NVMe`/`SSD`/`HDD`/
`Composite` → drive; `Ambient`/`SYSTIN`/`Mainboard`/`PCH` → system. An
unrecognised label on one of these chips is system, not other, because that is
what an unlabelled super-I/O channel almost always is.

The rest of `TemperatureSensorSelector` carries over unchanged:

- **Plausibility window**: 10 °C ≤ t < 125 °C, the same bounds
  (`minimumChipTemperature` is 10).
- **Primary selection**: the driver's package sensor — `Package id N`
  (coretemp), `Tctl`/`Tdie` (k10temp, zenpower), `Composite` (nvme), `edge`
  (amdgpu) — and, failing that, the hottest plausible reading of that kind.
  That fallback is `displayedCPUTemperature`'s, generalised from CPU to every
  kind the panel has a badge for.
- One primary per kind, so the CPU, GPU, drive and battery badges each have a
  default without the panel choosing.

**Thermal pressure**, the `ProcessInfo.ThermalState` replacement, is derived
from the trip points the drivers publish: the worst ratio of `temp*_input` to
`temp*_crit` (or `temp*_max`) across CPU and GPU sensors, bucketed at 0.75 /
0.85 / 0.95 into nominal / fair / serious / critical. It is recomputed by
`temperatures()` and read back with no I/O, and a change is announced through
`dispatch`.

**Fans** are `fan*_input`, with `fan*_min`/`fan*_max` where the driver has them
and -1 where it does not. `is_controllable` means a `pwm<N>` file exists beside
`fan<N>` (hwmon numbers them together); its path is reported so WP-C6's helper
is asked about a file this layer actually saw. Writing it is privileged and
lives in the helper. Nothing here writes.

### GPU (`IOAccelerator` "Device Utilization %")

macOS reads one number and every GPU answers it. Linux has no such number, and
the per-vendor differences are the feature's real shape; the measured matrix is
in [`docs/linux-port/SENSORS_BACKEND.md`](../../../docs/linux-port/SENSORS_BACKEND.md).
In outline: amdgpu answers everything from sysfs; NVIDIA answers everything
through NVML, which is `dlopen`ed and never linked so the AppImage starts
without the driver; Intel publishes **no** busy percentage in sysfs at all
(engine busyness is a perf PMU needing `CAP_PERFMON`, which the app must not
require), so `has_busy` stays false and the panel shows the GT clock instead of
inventing a percentage.

### Processes (`ProcessUsageService`)

| macOS | Linux |
|---|---|
| `proc_pid_rusage` cumulative user+system time | `/proc/<pid>/stat` `utime + stime`, converted with `sysconf(_SC_CLK_TCK)` |
| `MetricFormat.processCPUPercentage`: delta / (elapsed × cores) × 100 | the same arithmetic, in C, so the C and Swift answers agree |
| rows below 0.01 % dropped | `vs_process_query.minimum_cpu_percent`, default 0.01 |
| `phys_footprint` (excludes file-backed) | `RssAnon` from `/proc/<pid>/status`, with `VmRSS` and `VmSwap` alongside |
| grouped under the **responsible process**, so an app's helpers are one row | grouped under the **application identity**: the `.desktop` file whose `Exec` basename matches `argv[0]` (or, for a name the kernel truncated to 15 characters, `comm`), falling back to `argv[0]` and then `comm`. A browser's twenty renderers share one `comm` and so one row, which is the behaviour the responsible-process link gives on macOS |
| `ResponsibleProcess.displayName` | the `.desktop` `Name=`, else `argv[0]`'s basename, else `comm` |
| a gap longer than the interval × 4 resets the baseline | a gap longer than 30 s does, the same judgement `NetworkSampler.maxGap` makes at 10 s |

The `.desktop` index is built once per instance, from `XDG_DATA_HOME` and
`XDG_DATA_DIRS` (or, under a fixture root, from that tree's
`/usr/share/applications`, so a test is deterministic).

## The harness

```sh
vs-sensors caps        # capability flags and the chosen power backend
vs-sensors cpu         # per-core usage, load, topology, frequency
vs-sensors memory      # the panel's rows, and where the pressure came from
vs-sensors net         # counters, rates, kind, default route
vs-sensors disk        # per-device throughput, per-mount capacity
vs-sensors power       # battery, adapter, peripheral batteries
vs-sensors gpu         # per-card telemetry and what each vendor can answer
vs-sensors temps       # temperatures, classification, fans, thermal pressure
vs-sensors procs       # top processes, by cpu (default) or --sort memory
vs-sensors watch       # sample repeatedly and deliver events
```

Options: `--root PATH`, `--power upower|sysfs`, `--no-nvml`, `--interval SECS`,
`--count N`, `--limit N`, `--sort cpu|memory`, `--no-group`. Rate-bearing
commands default to `--count 2` so the deltas exist; `--count 1` prints the raw
counters only. Output is `key=value` under `[section]` headers, which is what
the shell tests assert on.

## Building and testing

```sh
cmake -S linux/platform -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build -R '^sensors' --output-on-failure
```

Build dependencies beyond the platform tree's own: `libsystemd-dev` (sd-bus,
for UPower). NVML is **not** a build dependency and must never become one.

The suites:

| suite | what it proves | how |
|---|---|---|
| `sensors_support` | the classification rules and every reader, exactly | C unit test over the three fixture trees |
| `sensors_fixture_amd` | a Ryzen desktop: k10temp, nvme, amdgpu, no battery, PSI | `vs-sensors --root` |
| `sensors_fixture_intel` | an Intel laptop: coretemp, i915 with no busy, a discharging battery, wifi, no PSI | `vs-sensors --root` |
| `sensors_fixture_thinkpad` | the awkward cases: label-driven chips, µAh batteries, a USB-PD charger, a peripheral battery | `vs-sensors --root` |
| `sensors_live_procfs` | invariants against this machine's real `/proc`, and that rates appear on a moving counter | `vs-sensors` with no root |
| `sensors_nvml` | the dlopen binding three ways: no library, a library whose init refuses, a library that answers | a stub `.so` with the same ABI |
| `sensors_upower_fake` | the sd-bus client, and capabilities shrinking when the daemon leaves | a fake UPower on a private bus |

`tests/fixtures/generate.py` regenerates the trees; they are checked in, so the
tests need no Python. Each file follows the format of the real kernel interface
it stands for — the field order of `/proc/stat` and `/proc/diskstats`, the units
of hwmon and power_supply, the attributes amdgpu and i915 publish — with
representative values that the tests name where they matter.

`scripts/build-matrix.sh` is the five-leg build the playbook requires: the four
CMake build types plus an ASan/UBSan/LSan leg whose suites must pass. It builds
the whole platform tree in every leg, so a warning this package introduces
elsewhere is caught here; it runs the rest of the tree's ctest once, minus the
capture and audio stack suites, which bring up sway, a portal and PipeWire and
cannot be trusted to come up cleanly on a shared container — those belong to
their own packages' matrices. The sanitizer leg additionally drives every
`vs-sensors` command against the live machine *and* against each fixture tree,
because the harness is where a missed `free_*` would hide and a bare container
exercises almost none of the allocating paths.

The recorded output of a full five-leg run is in
[`docs/linux-port/SENSORS_BACKEND.md`](../../../docs/linux-port/SENSORS_BACKEND.md).

A suite that needs teardown defines `test_cleanup`; it must not install its own
`EXIT` trap, because `harness.sh` owns the single one. (It used to install a
trap per `run`, which silently replaced the suite's and leaked a `dbus-daemon`
on every fake-UPower run.)
