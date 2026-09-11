#!/usr/bin/env python3
"""A minimal org.kde.StatusNotifierWatcher on the session bus.

The headless container has no panel, so nothing owns the watcher name and
QSystemTrayIcon reports the tray unavailable. This stands in for the panel:
it claims the name, implements the three members a toolkit actually reads
(RegisterStatusNotifierItem, RegisteredStatusNotifierItems,
IsStatusNotifierHostRegistered) and prints every registration it receives.
That is what proves a candidate really speaks StatusNotifierItem over D-Bus
rather than falling back to an X11 XEmbed icon.

Usage: sni-watcher.py [seconds]   -- prints "REGISTERED <service>" per item.
"""
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

WATCHER_BUS = "org.kde.StatusNotifierWatcher"
WATCHER_PATH = "/StatusNotifierWatcher"
WATCHER_IFACE = "org.kde.StatusNotifierWatcher"


class Watcher(dbus.service.Object):
    def __init__(self, bus):
        self.items = []
        dbus.service.Object.__init__(self, bus, WATCHER_PATH)

    @dbus.service.method(WATCHER_IFACE, in_signature="s", out_signature="")
    def RegisterStatusNotifierItem(self, service):
        self.items.append(str(service))
        print("REGISTERED %s" % service, flush=True)
        self.StatusNotifierItemRegistered(str(service))

    @dbus.service.method(WATCHER_IFACE, in_signature="s", out_signature="")
    def RegisterStatusNotifierHost(self, service):
        print("HOST %s" % service, flush=True)

    @dbus.service.signal(WATCHER_IFACE, signature="s")
    def StatusNotifierItemRegistered(self, service):
        pass

    # Qt reads the watcher through QDBusInterface, which builds its metaobject
    # from Introspect(). dbus-python does not put properties in the generated
    # XML, so without this override Qt sees no IsStatusNotifierHostRegistered
    # and concludes there is no tray. Hand-written XML it is.
    @dbus.service.method(dbus.INTROSPECTABLE_IFACE, in_signature="", out_signature="s")
    def Introspect(self):
        return (
            '<!DOCTYPE node PUBLIC "-//freedesktop//DTD D-BUS Object Introspection 1.0//EN"'
            ' "http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd">\n'
            "<node>\n"
            '  <interface name="org.freedesktop.DBus.Properties">\n'
            '    <method name="Get"><arg type="s" direction="in"/>'
            '<arg type="s" direction="in"/><arg type="v" direction="out"/></method>\n'
            '    <method name="GetAll"><arg type="s" direction="in"/>'
            '<arg type="a{sv}" direction="out"/></method>\n'
            "  </interface>\n"
            '  <interface name="%s">\n'
            '    <method name="RegisterStatusNotifierItem">'
            '<arg type="s" direction="in"/></method>\n'
            '    <method name="RegisterStatusNotifierHost">'
            '<arg type="s" direction="in"/></method>\n'
            '    <signal name="StatusNotifierItemRegistered"><arg type="s"/></signal>\n'
            '    <property name="RegisteredStatusNotifierItems" type="as" access="read"/>\n'
            '    <property name="IsStatusNotifierHostRegistered" type="b" access="read"/>\n'
            '    <property name="ProtocolVersion" type="i" access="read"/>\n'
            "  </interface>\n"
            "</node>\n" % WATCHER_IFACE
        )

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature="ss", out_signature="v")
    def Get(self, iface, prop):
        return self.GetAll(iface)[prop]

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature="s", out_signature="a{sv}")
    def GetAll(self, iface):
        return {
            "RegisteredStatusNotifierItems": dbus.Array(self.items, signature="s"),
            "IsStatusNotifierHostRegistered": dbus.Boolean(True),
            "ProtocolVersion": dbus.Int32(0),
        }


def main():
    seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 30
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    name = dbus.service.BusName(WATCHER_BUS, bus, do_not_queue=True)
    watcher = Watcher(bus)
    print("WATCHER UP", flush=True)
    loop = GLib.MainLoop()
    GLib.timeout_add_seconds(seconds, loop.quit)
    loop.run()
    print("WATCHER DOWN items=%d" % len(watcher.items), flush=True)
    del name


if __name__ == "__main__":
    main()
