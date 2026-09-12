#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# A fake org.freedesktop.UPower that speaks the documented interface: the
# DaemonVersion property, EnumerateDevices, and org.freedesktop.UPower.Device
# properties on each object, with the exact D-Bus signatures the real daemon
# uses (Percentage d, State u, Type u, EnergyRate d, TimeToEmpty x,
# Temperature d, Capacity d, ChargeCycles i, PowerSupply b, Online b).
#
# Real UPower cannot run in this container: there is no system bus and no
# battery. This stands in for it on a private bus, which is how the sd-bus
# client in power_upower.c gets exercised at all.

import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

UPOWER = "org.freedesktop.UPower"
DEVICE = "org.freedesktop.UPower.Device"
PROPERTIES = "org.freedesktop.DBus.Properties"
ROOT = "/org/freedesktop/UPower"

DEVICES = {
    ROOT + "/devices/line_power_AC": {
        "Type": dbus.UInt32(1),
        "PowerSupply": dbus.Boolean(True),
        "Online": dbus.Boolean(True),
        "NativePath": "AC",
        "Vendor": "",
        "Model": "",
    },
    ROOT + "/devices/battery_BAT0": {
        "Type": dbus.UInt32(2),
        "PowerSupply": dbus.Boolean(True),
        "NativePath": "BAT0",
        "Vendor": "ACME",
        "Model": "BAT-X1",
        "Percentage": dbus.Double(41.0),
        "State": dbus.UInt32(2),           # discharging
        "EnergyRate": dbus.Double(12.5),   # unsigned; the sign comes from State
        "TimeToEmpty": dbus.Int64(5400),
        "TimeToFull": dbus.Int64(0),
        "Temperature": dbus.Double(32.5),
        "Capacity": dbus.Double(91.0),     # percent of design, not a fraction
        "ChargeCycles": dbus.Int32(134),
        "Energy": dbus.Double(22.5),
        "EnergyFull": dbus.Double(52.0),
        "EnergyFullDesign": dbus.Double(57.0),
        "Voltage": dbus.Double(11.4),
    },
    ROOT + "/devices/mouse_hidpp_battery_0": {
        "Type": dbus.UInt32(5),            # mouse
        "PowerSupply": dbus.Boolean(False),
        "NativePath": "hidpp_battery_0",
        "Vendor": "Logitech",
        "Model": "MX Master 3",
        "Percentage": dbus.Double(80.0),
        "State": dbus.UInt32(2),
    },
    ROOT + "/devices/keyboard_hidpp_battery_1": {
        "Type": dbus.UInt32(6),            # keyboard
        "PowerSupply": dbus.Boolean(False),
        "NativePath": "hidpp_battery_1",
        "Vendor": "Logitech",
        "Model": "MX Keys",
        "Percentage": dbus.Double(25.0),
        "State": dbus.UInt32(2),
    },
}


class Device(dbus.service.Object):
    def __init__(self, bus, path, properties):
        super().__init__(bus, path)
        self.properties = properties

    @dbus.service.method(PROPERTIES, in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        if interface != DEVICE or name not in self.properties:
            raise dbus.exceptions.DBusException(
                "org.freedesktop.DBus.Error.InvalidArgs",
                "no such property: %s" % name)
        return self.properties[name]

    @dbus.service.method(PROPERTIES, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return self.properties if interface == DEVICE else {}


class Daemon(dbus.service.Object):
    def __init__(self, bus, path):
        super().__init__(bus, path)

    @dbus.service.method(UPOWER, in_signature="", out_signature="ao")
    def EnumerateDevices(self):
        return [dbus.ObjectPath(path) for path in DEVICES]

    @dbus.service.method(PROPERTIES, in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        if interface == UPOWER and name == "DaemonVersion":
            return dbus.String("1.90.2")
        if interface == UPOWER and name == "OnBattery":
            return dbus.Boolean(True)
        raise dbus.exceptions.DBusException(
            "org.freedesktop.DBus.Error.InvalidArgs", "no such property: %s" % name)

    @dbus.service.method(PROPERTIES, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        if interface == UPOWER:
            return {"DaemonVersion": dbus.String("1.90.2"),
                    "OnBattery": dbus.Boolean(True)}
        return {}


def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    name = dbus.service.BusName(UPOWER, bus)
    daemon = Daemon(bus, ROOT)
    devices = [Device(bus, path, properties) for path, properties in DEVICES.items()]
    # The test waits for this line before running the client.
    print("ready", flush=True)
    loop = GLib.MainLoop()
    try:
        loop.run()
    except KeyboardInterrupt:
        pass
    # Keep the objects alive for the lifetime of the loop.
    del name, daemon, devices
    return 0


if __name__ == "__main__":
    sys.exit(main())
