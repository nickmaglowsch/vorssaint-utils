#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# Regenerates the fixture trees next to this file. The trees themselves are
# checked in, so the tests need no Python; this script exists so a reviewer can
# see exactly what each file contains and why, and so a new machine layout can
# be added without hand-building sixty small files.
#
# Provenance: every file follows the format of the real kernel interface it
# stands for (the field order of /proc/stat and /proc/diskstats, the units of
# hwmon and power_supply, the attribute names amdgpu and i915 publish). The
# values are representative rather than captured verbatim, and where a value
# matters to a test the test names it.

import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def write(root, path, text):
    full = os.path.join(root, path.lstrip("/"))
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "w") as handle:
        handle.write(text if text.endswith("\n") else text + "\n")


def mkdir(root, path):
    os.makedirs(os.path.join(root, path.lstrip("/")), exist_ok=True)


def link(root, path, target):
    full = os.path.join(root, path.lstrip("/"))
    os.makedirs(os.path.dirname(full), exist_ok=True)
    if os.path.islink(full) or os.path.exists(full):
        os.remove(full)
    os.symlink(target, full)


def hwmon(root, index, name, device_path=None, files=None):
    """A hwmon node under /sys/devices, exposed at /sys/class/hwmon/hwmonN the
    way the kernel exposes it (a symlink into the device tree)."""
    base = device_path or "/sys/devices/virtual/hwmon/hwmon%d" % index
    write(root, base + "/name", name)
    for key, value in (files or {}).items():
        write(root, base + "/" + key, value)
    # The link lives in /sys/class/hwmon, so two levels up is /sys.
    assert base.startswith("/sys/")
    link(root, "/sys/class/hwmon/hwmon%d" % index, "../.." + base[len("/sys"):])
    return base


def cpu_topology(root, cpus, threads_per_core=2, khz=3400000, with_cpufreq=True):
    for index in range(cpus):
        base = "/sys/devices/system/cpu/cpu%d" % index
        write(root, base + "/topology/physical_package_id", "0")
        write(root, base + "/topology/core_id", str(index // threads_per_core))
        write(root, base + "/topology/thread_siblings_list",
              "%d,%d" % (index, index ^ 1) if threads_per_core == 2 else str(index))
        if with_cpufreq:
            write(root, base + "/cpufreq/scaling_cur_freq", str(khz + index * 1000))
            write(root, base + "/cpufreq/cpuinfo_max_freq", str(khz + 500000))


def proc_stat(root, cpus, busy_step=0):
    lines = []
    totals = [0] * 8
    for index in range(cpus):
        values = [
            100000 + index * 1000 + busy_step,  # user
            2000 + index * 10,                  # nice
            50000 + index * 500 + busy_step,    # system
            900000 + index * 100,               # idle
            4000,                               # iowait
            100,                                # irq
            900,                                # softirq
            0,                                  # steal
        ]
        totals = [a + b for a, b in zip(totals, values)]
        lines.append("cpu%d %s" % (index, " ".join(str(v) for v in values)))
    header = "cpu  %s" % " ".join(str(v) for v in totals)
    body = "\n".join([header] + lines)
    body += "\nintr 123456789\nctxt 987654321\nbtime 1700000000\nprocesses 54321\n"
    body += "procs_running 2\nprocs_blocked 0\n"
    write(root, "/proc/stat", body)


def proc_cpuinfo(root, cpus, model, mhz):
    blocks = []
    for index in range(cpus):
        blocks.append(
            "processor\t: %d\n"
            "vendor_id\t: GenuineIntel\n"
            "model name\t: %s\n"
            "cpu MHz\t\t: %.3f\n"
            "cache size\t: 16384 KB\n"
            "physical id\t: 0\n"
            "core id\t\t: %d\n"
            "cpu cores\t: %d\n" % (index, model, mhz + index, index // 2, cpus // 2))
    write(root, "/proc/cpuinfo", "\n".join(blocks))


def meminfo(root, total_kb, free_kb, available_kb, buffers_kb, cached_kb,
            sreclaimable_kb, shmem_kb, anon_kb, swap_total_kb, swap_free_kb,
            zswapped_kb=None):
    lines = [
        ("MemTotal", total_kb), ("MemFree", free_kb), ("MemAvailable", available_kb),
        ("Buffers", buffers_kb), ("Cached", cached_kb), ("SwapCached", 0),
        ("Active", anon_kb + cached_kb // 2), ("Inactive", cached_kb // 2),
        ("Unevictable", 4096), ("Mlocked", 4096),
        ("SwapTotal", swap_total_kb), ("SwapFree", swap_free_kb),
        ("Dirty", 512), ("Writeback", 0), ("AnonPages", anon_kb),
        ("Mapped", 512000), ("Shmem", shmem_kb), ("KReclaimable", sreclaimable_kb),
        ("Slab", sreclaimable_kb + 120000), ("SReclaimable", sreclaimable_kb),
        ("SUnreclaim", 120000), ("KernelStack", 20000), ("PageTables", 60000),
    ]
    if zswapped_kb is not None:
        lines.append(("Zswap", zswapped_kb // 3))
        lines.append(("Zswapped", zswapped_kb))
    write(root, "/proc/meminfo",
          "\n".join("%-15s%9d kB" % (key + ":", value) for key, value in lines))


NET_HEADER = (
    "Inter-|   Receive                                                |  Transmit\n"
    " face |bytes    packets errs drop fifo frame compressed multicast|"
    "bytes    packets errs drop fifo colls carrier compressed")


def net_dev(root, interfaces):
    lines = [NET_HEADER]
    for name, rx, tx in interfaces:
        lines.append("%6s: %d %d 0 0 0 0 0 0 %d %d 0 0 0 0 0 0" %
                     (name, rx, rx // 1000, tx, tx // 1000))
    write(root, "/proc/net/dev", "\n".join(lines))


def net_class(root, name, kind, up=True, pci="0000:00:1f.6", driver="e1000e"):
    """`device` and `phy80211` are symlinks into the device tree on a real
    machine, and they are symlinks here too: git stores no empty directory, so
    a fixture that relied on a bare `mkdir` would vanish on clone and the
    interface would silently reclassify as virtual."""
    base = "/sys/class/net/" + name
    write(root, base + "/operstate", "up" if up else "down")
    if kind == "loopback":
        write(root, base + "/type", "772")
        return
    write(root, base + "/type", "1")
    if kind in ("wifi", "ethernet"):
        device = "/sys/devices/pci0000:00/" + pci
        write(root, device + "/uevent", "DRIVER=%s\nPCI_SLOT_NAME=%s" % (driver, pci))
        link(root, base + "/device", "../../../devices/pci0000:00/" + pci)
    if kind == "wifi":
        phy = "/sys/devices/virtual/ieee80211/phy0"
        write(root, phy + "/name", "phy0")
        link(root, base + "/phy80211", "../../../devices/virtual/ieee80211/phy0")
    # "virtual" gets neither a device nor a phy80211, which is what makes it
    # virtual: there is no hardware behind it.


def net_route(root, interface):
    write(root, "/proc/net/route",
          "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
          "%s\t00000000\t0102A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0\n"
          "%s\t0002A8C0\t00000000\t0001\t0\t0\t100\t00FFFFFF\t0\t0\t0" % (interface, interface))


def diskstats(root, devices):
    lines = []
    for major, minor, name, reads, read_sectors, writes, write_sectors, ticks in devices:
        lines.append("%4d %7d %s %d 0 %d 0 %d 0 %d 0 0 %d 0 0 0 0 0 0" %
                     (major, minor, name, reads, read_sectors, writes, write_sectors, ticks))
    write(root, "/proc/diskstats", "\n".join(lines))


def mounts(root, rows):
    write(root, "/proc/self/mounts",
          "\n".join("%s %s %s %s 0 0" % row for row in rows))


def power_supply(root, name, values):
    for key, value in values.items():
        write(root, "/sys/class/power_supply/%s/%s" % (name, key), str(value))


# --------------------------------------------------------------------- trees


def build_amd(root):
    """Ryzen desktop: k10temp, an nvme drive, an amdgpu card, no battery, PSI on."""
    proc_stat(root, 8)
    write(root, "/proc/loadavg", "1.42 0.98 0.61 2/812 24601")
    proc_cpuinfo(root, 8, "AMD Ryzen 7 5800X 8-Core Processor", 3800.0)
    meminfo(root, 32768000, 18000000, 24000000, 260000, 6200000, 700000, 340000,
            7100000, 8388604, 8388604, zswapped_kb=0)
    write(root, "/proc/pressure/memory",
          "some avg10=0.42 avg60=0.18 avg300=0.05 total=1234567\n"
          "full avg10=0.11 avg60=0.04 avg300=0.01 total=234567")
    write(root, "/proc/pressure/cpu",
          "some avg10=2.10 avg60=1.40 avg300=0.90 total=9876543")
    cpu_topology(root, 8, threads_per_core=2, khz=3800000)

    net_dev(root, [("lo", 512000, 512000), ("enp5s0", 91827364, 12345678),
                   ("virbr0", 0, 0)])
    net_class(root, "lo", "loopback")
    net_class(root, "enp5s0", "ethernet", pci="0000:05:00.0", driver="r8169")
    net_class(root, "virbr0", "virtual")
    net_route(root, "enp5s0")

    diskstats(root, [
        (259, 0, "nvme0n1", 1204513, 61254144, 884210, 40551232, 921444),
        (259, 1, "nvme0n1p1", 1520, 61440, 12, 1024, 940),
        (259, 2, "nvme0n1p2", 1202993, 61192704, 884198, 40550208, 920504),
        (8, 0, "sda", 4120, 401920, 22140, 3840512, 41200),
        (7, 0, "loop0", 0, 0, 0, 0, 0),
    ])
    write(root, "/sys/class/block/nvme0n1p1/partition", "1")
    write(root, "/sys/class/block/nvme0n1p2/partition", "2")
    # Whole disks have no `partition` file; the `dev` node number is there so
    # the directory exists in git, which stores no empty directory.
    write(root, "/sys/class/block/nvme0n1/dev", "259:0")
    write(root, "/sys/class/block/sda/dev", "8:0")
    mounts(root, [
        ("proc", "/proc", "proc", "rw,relatime"),
        ("sysfs", "/sys", "sysfs", "rw,relatime"),
        ("tmpfs", "/run", "tmpfs", "rw,nosuid,nodev"),
        ("/dev/nvme0n1p2", "/", "ext4", "rw,relatime"),
        ("/dev/nvme0n1p1", "/boot/efi", "vfat", "rw,relatime"),
        ("/dev/sda", "/mnt/data", "xfs", "ro,relatime"),
        ("cgroup2", "/sys/fs/cgroup", "cgroup2", "rw,nosuid"),
    ])

    hwmon(root, 0, "k10temp", "/sys/devices/pci0000:00/0000:00:18.3/hwmon/hwmon0", {
        "temp1_input": "48375", "temp1_label": "Tctl",
        "temp2_input": "44250", "temp2_label": "Tccd1",
        "temp1_max": "95000", "temp1_crit": "100000",
    })
    hwmon(root, 1, "nvme", "/sys/devices/pci0000:00/0000:01:00.0/hwmon/hwmon1", {
        "temp1_input": "41850", "temp1_label": "Composite",
        "temp1_crit": "84850", "temp1_max": "81850",
        "temp2_input": "39850", "temp2_label": "Sensor 1",
    })
    gpu_hwmon = "/sys/devices/pci0000:00/0000:03:00.0/hwmon/hwmon2"
    hwmon(root, 2, "amdgpu", gpu_hwmon, {
        "temp1_input": "52000", "temp1_label": "edge", "temp1_crit": "100000",
        "temp2_input": "61000", "temp2_label": "junction", "temp2_crit": "110000",
        "temp3_input": "58000", "temp3_label": "mem", "temp3_crit": "105000",
        "power1_average": "43000000", "power1_cap": "230000000",
        "fan1_input": "1180", "fan1_min": "0", "fan1_max": "3200",
        "pwm1": "94", "pwm1_enable": "2",
        "freq1_input": "2405000000",
    })
    device = "/sys/devices/pci0000:00/0000:03:00.0"
    write(root, device + "/uevent",
          "DRIVER=amdgpu\nPCI_CLASS=30000\nPCI_ID=1002:73BF\nPCI_SLOT_NAME=0000:03:00.0")
    write(root, device + "/vendor", "0x1002")
    write(root, device + "/device", "0x73bf")
    write(root, device + "/gpu_busy_percent", "37")
    write(root, device + "/mem_info_vram_used", "2415919104")
    write(root, device + "/mem_info_vram_total", "17163091968")
    mkdir(root, "/sys/class/drm/card0")
    link(root, "/sys/class/drm/card0/device", "../../../devices/pci0000:00/0000:03:00.0")
    # A connector, not a device: the GPU walk must skip anything with a dash.
    write(root, "/sys/class/drm/card0-DP-1/status", "connected")

    # A desktop: the class directory exists and is empty, which is not the same
    # as it being absent. (.gitkeep only makes git carry the directory; the
    # backend skips dotfiles, so it still sees an empty class.)
    write(root, "/sys/class/power_supply/.gitkeep", "")
    write(root, "/usr/share/applications/firefox.desktop",
          "[Desktop Entry]\nType=Application\nName=Firefox\nExec=/usr/lib/firefox/firefox %u")


def build_intel(root):
    """Laptop: coretemp + acpitz, an i915 GT, a discharging BAT0, wifi, no PSI."""
    proc_stat(root, 4)
    write(root, "/proc/loadavg", "0.31 0.44 0.52 1/402 8812")
    proc_cpuinfo(root, 4, "Intel(R) Core(TM) i7-1165G7 @ 2.80GHz", 1200.0)
    meminfo(root, 16384000, 900000, 9100000, 180000, 6800000, 420000, 620000,
            5900000, 4194300, 4090000)
    cpu_topology(root, 4, threads_per_core=2, khz=1200000)

    net_dev(root, [("lo", 12000, 12000), ("wlan0", 4455667788, 998877665),
                   ("docker0", 0, 0)])
    net_class(root, "lo", "loopback")
    net_class(root, "wlan0", "wifi", pci="0000:00:14.3", driver="iwlwifi")
    net_class(root, "docker0", "virtual", up=False)
    net_route(root, "wlan0")
    write(root, "/proc/net/ipv6_route",
          "00000000000000000000000000000000 00 00000000000000000000000000000000 00 "
          "fe800000000000000000000000000001 00000400 00000001 00000000 00000003 wlan0")

    diskstats(root, [
        (259, 0, "nvme0n1", 804120, 40206000, 512044, 20481024, 411000),
        (259, 1, "nvme0n1p1", 2000, 80000, 40, 2048, 1200),
    ])
    write(root, "/sys/class/block/nvme0n1p1/partition", "1")
    mounts(root, [
        ("sysfs", "/sys", "sysfs", "rw"),
        ("/dev/nvme0n1p2", "/", "btrfs", "rw,relatime,compress=zstd:1"),
        ("/dev/nvme0n1p1", "/boot", "ext4", "rw,relatime"),
        ("tmpfs", "/dev/shm", "tmpfs", "rw"),
        ("gvfsd-fuse", "/run/user/1000/gvfs", "fuse.gvfsd-fuse", "rw,nosuid"),
    ])

    hwmon(root, 0, "coretemp", "/sys/devices/platform/coretemp.0/hwmon/hwmon0", {
        "temp1_input": "56000", "temp1_label": "Package id 0",
        "temp1_max": "100000", "temp1_crit": "100000",
        "temp2_input": "54000", "temp2_label": "Core 0",
        "temp2_max": "100000", "temp2_crit": "100000",
        "temp3_input": "57000", "temp3_label": "Core 1",
        "temp3_max": "100000", "temp3_crit": "100000",
    })
    hwmon(root, 1, "acpitz", "/sys/devices/virtual/thermal/thermal_zone0/hwmon/hwmon1", {
        "temp1_input": "43000", "temp1_crit": "107000",
    })
    hwmon(root, 2, "BAT0", "/sys/devices/LNXSYSTM:00/PNP0C0A:00/power_supply/BAT0/hwmon/hwmon2", {
        "temp1_input": "31200",
    })

    device = "/sys/devices/pci0000:00/0000:00:02.0"
    write(root, device + "/uevent", "DRIVER=i915\nPCI_ID=8086:9A49\nPCI_SLOT_NAME=0000:00:02.0")
    write(root, device + "/vendor", "0x8086")
    write(root, device + "/device", "0x9a49")
    mkdir(root, "/sys/class/drm/card0")
    link(root, "/sys/class/drm/card0/device", "../../../devices/pci0000:00/0000:00:02.0")
    write(root, "/sys/class/drm/card0/gt/gt0/rps_cur_freq_mhz", "950")
    write(root, "/sys/class/drm/card0/gt/gt0/rps_max_freq_mhz", "1300")

    power_supply(root, "BAT0", {
        "type": "Battery", "status": "Discharging", "present": 1, "capacity": 62,
        "capacity_level": "Normal", "cycle_count": 214,
        "energy_now": 31460000, "energy_full": 50720000, "energy_full_design": 57000000,
        "power_now": 8940000, "voltage_now": 11550000, "voltage_min_design": 11520000,
        "manufacturer": "SMP", "model_name": "5B10W51", "serial_number": "1234",
        "technology": "Li-poly", "scope": "System",
    })
    power_supply(root, "AC", {"type": "Mains", "online": 0})
    write(root, "/usr/share/applications/org.gnome.Nautilus.desktop",
          "[Desktop Entry]\nType=Application\nName=Files\nExec=nautilus --new-window %U")


def build_thinkpad(root):
    """ThinkPad: a label-driven platform chip, charge_* units, a USB-PD charger
    and a wireless mouse with its own power_supply node."""
    proc_stat(root, 2)
    write(root, "/proc/loadavg", "0.05 0.10 0.14 1/210 3312")
    proc_cpuinfo(root, 2, "Intel(R) Core(TM) i5-8250U CPU @ 1.60GHz", 800.0)
    meminfo(root, 8192000, 300000, 4100000, 90000, 3400000, 210000, 260000,
            3200000, 0, 0)
    cpu_topology(root, 2, threads_per_core=2, khz=800000, with_cpufreq=False)
    net_dev(root, [("lo", 1000, 1000)])
    net_class(root, "lo", "loopback")
    diskstats(root, [(8, 0, "sda", 100000, 4000000, 50000, 2000000, 90000)])
    mounts(root, [("/dev/sda1", "/", "ext4", "rw,relatime")])

    hwmon(root, 0, "thinkpad", "/sys/devices/platform/thinkpad_hwmon/hwmon/hwmon0", {
        "temp1_input": "51000", "temp1_label": "CPU",
        "temp2_input": "47000", "temp2_label": "GPU",
        "temp3_input": "32000", "temp3_label": "Bat0",
        "temp4_input": "38000", "temp4_label": "Ambient",
        "fan1_input": "2914", "fan1_label": "Fan 1",
        "pwm1": "128", "pwm1_enable": "1",
        "fan2_input": "0", "fan2_label": "Fan 2",
    })
    hwmon(root, 1, "dell_smm", "/sys/devices/platform/dell_smm_hwmon/hwmon/hwmon1", {
        "temp1_input": "49000", "temp1_label": "CPU",
        "fan1_input": "2680", "fan1_label": "Processor Fan",
    })
    hwmon(root, 2, "asus", "/sys/devices/platform/asus-nb-wmi/hwmon/hwmon2", {
        "temp1_input": "45000", "temp1_label": "CPU Temperature",
        "fan1_input": "3100", "fan1_label": "CPU Fan",
    })
    hwmon(root, 3, "zenpower", "/sys/devices/pci0000:00/0000:00:18.3/hwmon/hwmon3", {
        "temp1_input": "53125", "temp1_label": "Tdie", "temp1_crit": "95000",
        "temp2_input": "56000", "temp2_label": "Tctl",
    })
    hwmon(root, 4, "nouveau", "/sys/devices/pci0000:00/0000:01:00.0/hwmon/hwmon4", {
        "temp1_input": "64000", "temp1_crit": "105000",
        "fan1_input": "1750",
    })

    power_supply(root, "BAT0", {
        "type": "Battery", "status": "Charging", "present": 1,
        "charge_now": 3120000, "charge_full": 4210000, "charge_full_design": 4800000,
        "voltage_now": 12480000, "current_now": 1900000, "cycle_count": 87,
        "manufacturer": "LGC", "model_name": "01AV445", "scope": "System",
        "temp": 312,
    })
    power_supply(root, "ucsi-source-psy-USBC000:001", {
        "type": "USB_PD", "online": 1, "voltage_now": 20000000, "current_now": 2250000,
        "voltage_max_design": 20000000, "current_max": 3250000,
    })
    power_supply(root, "hid-e2:05:a1:57:3c:9b-battery", {
        "type": "Battery", "scope": "Device", "status": "Discharging",
        "capacity": 55, "model_name": "MX Master 3 Mouse", "manufacturer": "Logitech",
        "present": 1,
    })
    write(root, "/usr/share/applications/code.desktop",
          "[Desktop Entry]\nType=Application\nName=Code\nExec=/usr/share/code/code %F")


TREES = {
    "amd-desktop": build_amd,
    "intel-laptop": build_intel,
    "thinkpad": build_thinkpad,
}


def main():
    for name, builder in TREES.items():
        root = os.path.join(HERE, name)
        if os.path.isdir(root):
            shutil.rmtree(root)
        os.makedirs(root)
        builder(root)
        print("built", root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
