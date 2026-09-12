# Sensors backend: measured evidence and vendor matrix

The backend for WP-A1 to WP-A4 (`linux/platform/sensors`), what was measured
and where, and the per-vendor GPU matrix WP-A3 asks for. The API and the
per-metric macOS → Linux mapping live in
[`linux/platform/sensors/README.md`](../../linux/platform/sensors/README.md);
this file is the evidence behind them.

## What the container could and could not prove

The development container has a real `/proc` and a partial `/sys` and nothing
else: no hwmon, no battery, no GPU, no PSI, no system bus. Every reader is
therefore rooted at `vs_sensors_options.root`, and the suites split in two:

```
$ ls /sys/class/hwmon /sys/class/drm
ls: cannot access '/sys/class/hwmon': No such file or directory
ls: cannot access '/sys/class/drm': No such file or directory
$ ls /sys/class/power_supply
                       # exists, empty
$ ls /proc/pressure
ls: cannot access '/proc/pressure': No such file or directory
```

| verified **live** here | verified against a **fixture** | not verifiable here |
|---|---|---|
| `/proc/stat` aggregate and per-core deltas, `/proc/loadavg`, `/proc/cpuinfo` frequency fallback, CPU topology | frequency from `cpufreq/scaling_cur_freq`; multi-package and SMT topology | — |
| `/proc/meminfo` parse, the MemAvailable pressure fallback, zswap detection | the PSI path, a non-zero compressor, swap in use | — |
| `/proc/net/dev` counters and rates, loopback classification, the default-route read | wifi (`phy80211`), virtual (no `device` link), IPv6 default route | a real wireless NIC |
| `/proc/diskstats` counters and rates, `/proc/self/mounts` filtering, `statvfs` | partition marking, read-only mounts, the device join | — |
| `/proc/<pid>/stat` and `status` for every live process, the delta policy, `.desktop` name resolution | — | — |
| — | the whole hwmon walk and classification, fans and pwm detection, thermal pressure | a real thermal trip being crossed |
| — | the `/sys/class/power_supply` path: energy and charge units, health, adapter watts, USB-PD rating, peripheral batteries | a real charge/discharge transition |
| the sd-bus UPower client, against a fake daemon on a private bus | — | real UPower with a real battery |
| the NVML `dlopen` binding, against a stub with the same ABI; the no-driver and failed-init paths | — | a real NVIDIA driver |
| — | amdgpu busy/VRAM/power/temperature/clock; Intel GT clock and the absence of a busy percentage | any real GPU |

Nothing in the table's first column is a fixture, and nothing in the second is
claimed as a live measurement.

## Measurements

All commands run from the worktree root, on the container described above
(Ubuntu 24.04, 4 vCPU, kernel 6.18 microVM).

### Build and tests: the five-leg matrix

`linux/platform/sensors/scripts/build-matrix.sh`, verbatim:

```
===== default =====
  -- No CMAKE_BUILD_TYPE given, defaulting to RelWithDebInfo
  CMAKE_BUILD_TYPE = RelWithDebInfo
  build: clean, no compiler warnings
  sensors ctest: 100% tests passed, 0 tests failed out of 7
  rest of the tree (no session stack): 100% tests passed, 0 tests failed out of 19

===== Debug =====
  CMAKE_BUILD_TYPE = Debug
  build: clean, no compiler warnings
  sensors ctest: 100% tests passed, 0 tests failed out of 7

===== Release =====
  CMAKE_BUILD_TYPE = Release
  build: clean, no compiler warnings
  sensors ctest: 100% tests passed, 0 tests failed out of 7

===== RelWithDebInfo =====
  CMAKE_BUILD_TYPE = RelWithDebInfo
  build: clean, no compiler warnings
  sensors ctest: 100% tests passed, 0 tests failed out of 7

===== sanitizers =====
  CMAKE_BUILD_TYPE = Debug
  build: clean, no compiler warnings
  sensors ctest: 100% tests passed, 0 tests failed out of 7
  sanitizers: no leak, no undefined behaviour in any sensors suite
  vs-sensors caps: clean
  vs-sensors cpu: clean
  vs-sensors memory: clean
  vs-sensors net: clean
  vs-sensors disk: clean
  vs-sensors power: clean
  vs-sensors gpu: clean
  vs-sensors temps: clean
  vs-sensors procs: clean
  vs-sensors --root amd-desktop: all commands clean
  vs-sensors --root intel-laptop: all commands clean
  vs-sensors --root thinkpad: all commands clean

===== summary =====
all five configurations build clean under -Werror and pass the sensors suites
```

Clean under `-Wall -Wextra -Werror -Wshadow -Wstrict-prototypes
-Wmissing-prototypes -Wpointer-arith -Wwrite-strings` in every leg. The whole
platform tree is *built* in all five, which is what catches a warning this
package introduces elsewhere; it is *run* once, minus the capture and audio
stack suites, which bring up sway, a portal and PipeWire and cannot be trusted
to come up cleanly on a container several agents have shared.

The sanitizer leg matters here more than in most packages: this is string and
buffer handling over files it did not write, and every list it returns is freed
by a matching `free_*` member that an exit path could skip. It drives every
`vs-sensors` command against the live machine *and* against each fixture tree,
because a bare container exercises almost none of the allocating paths — no
hwmon list, no battery, no GPU, no fans.

`test_sensors_support` alone is 248 assertions over the three fixture trees.

One defect the sanitizer leg did not find, and a count did: the shell harness
installed an `EXIT` trap on every `run` to delete its temp file, silently
replacing whatever the test had installed. The fake-UPower suite's own teardown
was the casualty, so every invocation left a `dbus-daemon` running. Fixed by
giving the harness one trap and a `test_cleanup` hook; verified by running the
suite twice and counting the daemons (15 before, 15 after).

### Live CPU, this container

```
$ build/sensors/vs-sensors --no-nvml --count 2 --interval 0.5 cpu
[cpu]
has_rates=1
interval_seconds=0.500
total_usage=0.0101
user_usage=0.0000
system_usage=0.0101
idle_usage=0.9899
iowait_usage=0.0000
load1=0.57
load5=0.28
load15=0.22
core_count=4
physical_core_count=4
package_count=1
core0 usage=0.0200 mhz=2100.0 package=0 core_id=0
core1 usage=0.0000 mhz=2100.0 package=0 core_id=1
core2 usage=0.0392 mhz=2100.0 package=0 core_id=2
core3 usage=0.0000 mhz=2100.0 package=0 core_id=3
```

Four logical and four physical cores, one package: this microVM exposes no SMT.
The frequency is the `/proc/cpuinfo` fallback, because the container has no
`cpufreq` tree — the first case the fallback exists for, found live rather than
designed for.

### Live memory, this container

```
$ build/sensors/vs-sensors --no-nvml memory
[memory]
total_bytes=16856092672
used_bytes=1102303232
app_bytes=495980544
cached_bytes=4220219392
available_bytes=15753789440
has_compressed=1
compressed_bytes=0
swap_total_bytes=0
swap_used_bytes=0
pressure=0.0654
pressure_full=0.0000
pressure_is_psi=0
```

No `/proc/pressure`, so the pressure is the MemAvailable shortfall and says so
(`pressure_is_psi=0`). A UI that treated the two as the same number would be
wrong by construction, which is why the flag is in the struct rather than in a
log line.

The `used`/`app`/`cached` split is the mapping working: ~1.0 GiB in use against
~3.9 GiB of reclaimable page cache. `used_bytes` is the same quantity `free`
prints in its `used` column (procps-ng 4.0.4 here), for the same reason
Activity Monitor excludes cached files from Memory Used — the identity is
checked, not assumed:

```
$ free -m | head -2
               total        used        free      shared  buff/cache   available
Mem:           16075        1003       11375          32        4058       15071
$ grep -E "^(MemTotal|MemAvailable):" /proc/meminfo    # the same moment
MemTotal:       16461028 kB
MemAvailable:   15433432 kB                            # 16461028-15433432 = 1003 MiB
```

The naive "total minus free" would instead have called ~3.9 GiB of droppable
cache "used". This
kernel does publish a `Zswapped` line (at zero), so `has_compressed` is set and
the compressed row would appear; on a kernel without zswap it is absent and the
row is hidden rather than pinned at zero.

### Cost

The per-call cost matters because the monitor samples on a timer and
`MonitorSamplingPolicy` budgets for it. Measured as the median of 30 runs of
`vs-sensors --no-nvml --count 1 <command>` minus the median of 30 runs of
`caps`, which subtracts process start and capability probing and leaves the
call itself. Live, on this container (4 vCPU, 114 processes, 4 interfaces,
15 block devices, 7 mounted filesystems):

| call | cost | notes |
|---|---|---|
| `memory` | 0.15 ms | one file |
| `gpu` | 0.13 ms | no cards here; the walk finds nothing |
| `temps` + `fans` + `thermal_pressure` | 0.16 ms | no hwmon here; the walk finds nothing |
| `network` | 0.30 ms | one file plus three or four small sysfs reads per interface |
| `cpu` | 0.34 ms | `/proc/stat`, `/proc/loadavg`, `/proc/cpuinfo`, plus one frequency read per core. Topology is read once per instance, not per sample |
| `disk_devices` + `disk_mounts` | 0.51 ms | `statvfs` per mount is most of it |
| `processes` | 2.68 ms | three files per pid, top-N over all of them |

`processes` is an order of magnitude dearer than the rest, and it is the call
the macOS side also treats as expensive: `ProcessUsageService` caches rows for
18 seconds and refuses to resample faster than the sampling interval. The Swift
side keeps that policy unchanged — this measurement says it should, not that it
can be dropped. The `temps` and `gpu` figures are floors, not costs on a machine
that has those devices; those are the fixture trees' territory and were not
timed on real hardware.

## GPU vendor matrix

What each vendor can actually answer, unprivileged, and what the panel therefore
shows. `✓` = read and verified against the named source; `✗` = the interface
does not exist; `helper` = exists but needs privilege the app must not take.

| | AMD (`amdgpu`) | Intel (`i915`/`xe`) | NVIDIA (NVML) | macOS |
|---|---|---|---|---|
| busy % | ✓ `device/gpu_busy_percent` | ✗ | ✓ `nvmlDeviceGetUtilizationRates` | ✓ IOAccelerator |
| VRAM used / total | ✓ `mem_info_vram_used` / `_total` | ✗ (integrated: shared with system RAM) | ✓ `nvmlDeviceGetMemoryInfo` | ✓ |
| temperature | ✓ hwmon `temp1..3_input` (edge, junction, mem) | ◐ hwmon on discrete cards only | ✓ `nvmlDeviceGetTemperature` | ✓ |
| power draw | ✓ hwmon `power1_average` | ◐ hwmon `energy1_input` on discrete only, and it is a counter, not a rate | ✓ `nvmlDeviceGetPowerUsage` | ✓ |
| clock | ✓ hwmon `freq1_input` | ✓ `gt/gt0/rps_cur_freq_mhz`, or `gt_cur_freq_mhz` before 6.2 | ✓ `nvmlDeviceGetClockInfo` | ✗ |
| per-process GPU | ✗ (fdinfo, unaggregated) | ✗ | ◐ NVML, and not on consumer drivers | ✓ |
| needs a driver install | ✗ in-tree | ✗ in-tree | **✓ proprietary** | — |

**Intel's missing busy percentage is the one honest gap.** Engine busyness is
published through a perf PMU (`i915`/`xe` events), which needs either
`CAP_PERFMON` on the binary or `kernel.perf_event_paranoid <= 0` system-wide —
that is how `intel_gpu_top` does it, and why it asks for root. Requiring either
to draw a percentage is out of proportion to the feature, so `has_busy` stays
false, the panel shows the GT frequency instead, and the capabilities page says
why. Nothing is invented from the clock: a frequency is not a utilisation, and
presenting it as one would be the dishonest version of this row.

**NVIDIA is `dlopen`-only, by construction.** There is no `-lnvidia-ml`
anywhere in the tree, no vendored NVML header, and the ten entry points are
resolved by name against the ABI NVIDIA has published unchanged since driver
340. The AppImage must start on a machine with no NVIDIA driver, so all three
paths are tested: no library, a library whose `nvmlInit` refuses (the
driver/library version mismatch every NVIDIA user has met), and a library that
answers. The test asserts that `vs-sensors` has no `NEEDED` entry for
libnvidia-ml, so the rule cannot quietly stop being true.

**amdgpu is the good case** and, conveniently, the one that matters most for a
desktop with a discrete card.

## Temperature classification: what replaces the SMC key table

`TemperatureSensorSelector` maps SMC keys to "this is the CPU" per Apple
Silicon generation. The Linux replacement is a driver-name table with a
label-driven escape hatch, because the hwmon `name` file is the only stable
identity a sensor has. The rules, the fixture that covers each, and the reason:

| driver | kind | covered by | note |
|---|---|---|---|
| `coretemp` | CPU | intel-laptop | `Package id 0` is the package sensor |
| `k10temp` | CPU | amd-desktop | `Tctl` is the package sensor, `Tccd1` is not |
| `zenpower` | CPU | thinkpad | out-of-tree driver, same shape as k10temp |
| `nvme` | drive | amd-desktop | `Composite` is the package sensor |
| `acpitz` | system | intel-laptop | an ACPI thermal zone is ambient, not the CPU |
| `amdgpu` | GPU | amd-desktop | `edge`, `junction`, `mem` all GPU; `edge` is primary |
| `nouveau` | GPU | thinkpad | no labels at all, so the name has to carry it |
| `thinkpad` | **label** | thinkpad | one chip publishes CPU, GPU, Bat0 and Ambient |
| `dell_smm` | **label** | thinkpad | same |
| `asus` | **label** | thinkpad | same |
| `BAT*`, `bq27*` | battery | intel-laptop | the battery's own hwmon node |

An unrecognised label on a label-driven chip is classified `system`, not
`other`: an unlabelled super-I/O channel is a board sensor far more often than
it is anything else, and `other` would hide it from every badge.

Carried over unchanged from `TemperatureSensorSelector`: the 10 °C ≤ t < 125 °C
plausibility window, and the "hottest plausible reading of its kind" fallback
when no package sensor is present — the fallback that exists because not every
Mac carries the sensors its chip generation is mapped to, and which is just as
necessary here, where not every board labels anything.

**Thermal pressure** (the `ProcessInfo.ThermalState` replacement) is the worst
ratio of `temp*_input` to `temp*_crit` — or `temp*_max` where there is no crit —
across CPU and GPU sensors, bucketed at 0.75 / 0.85 / 0.95. Sensors with no
trip point are excluded rather than assumed. This is the one number here with no
kernel source behind it, and it is a policy choice: it is stated in one function
(`vs_thermal_from_ratio`), unit-tested at each boundary, and easy to retune when
WP-C6's fan curves have real hardware to calibrate against.

## Decisions the lead should confirm

1. **`MemorySample.activeBytes`'s doc comment is wrong.**
   `Sources/VorssaintCore/Platform/SystemSensors.swift` says `activeBytes` is
   "`MemTotal - MemAvailable` on Linux". That quantity is *memory in use*
   (macOS's Memory Used); `activeBytes` is documented as macOS's *app memory*,
   whose Linux counterpart is `AnonPages`. The C layer publishes both under
   names that say what they are (`used_bytes`, `app_bytes`); the Swift comment
   needs a one-line correction, which is WP-12's file, not this package's.

2. **Memory pressure needs its provenance in the UI.** With PSI it is a stall
   fraction directly comparable to `kern.memorystatus_vm_pressure_level`;
   without it, it is a fill level and a different thing. `pressure_is_psi` is in
   the struct; whether the panel shows a different label, or hides the
   traffic-light entirely on a kernel without PSI, is a design call.

3. **SMART and NVMe health are out of scope and stay out** until a helper method
   exists (WP-S1 / `PRIVILEGES.md`). `DiskSMARTReading` has nine fields the
   macOS panel fills from unprivileged IOKit properties; on Linux every one of
   them needs `SG_IO` or `NVME_IOCTL_ADMIN_CMD`, i.e. root. The rows should be
   hidden on Linux rather than shown empty. **That arguably makes monitorDisk
   "Reduced" rather than "Port" in `FEATURE_TRIAGE.md`.** Changing a feature's
   verdict is the lead's call, not an executor's, so the row still says Port
   and names this decision instead.

4. **Disk eject and removability are not in this backend.** They are UDisks2,
   which is a D-Bus service with its own policy, and belong with the panel work
   rather than with an unprivileged sysfs reader.

5. **Per-process disk I/O is probed but not read.** `has_procfs_io` reports
   whether `/proc/<pid>/io` is readable; filling the rows is panel work and was
   left to WP-A1 rather than guessed at here.

## Support matrix rows

The desktop environment makes no difference to any of this: `/proc`, `/sys`,
UPower and NVML are the same under GNOME, KDE, wlroots and X11, which is why
`FEATURE_TRIAGE.md`'s sensors rows are ✓ across the board. The variation that
does matter is hardware and kernel configuration, and it is carried by the
capability flags: `has_hwmon`, `has_upower`, `has_power_supply`, `has_amdgpu`,
`has_nvml`, `has_intel_gpu`, `has_psi`, `has_procfs_io`.
