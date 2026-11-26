#!/usr/bin/env python3
# Test helper (#123): a stand-in StatusNotifierWatcher with a registered host for the nested
# session's private bus, so KStatusNotifierItem registers its item. Logs registrations to argv[1].
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

IFACE = "org.kde.StatusNotifierWatcher"
log_path = sys.argv[1]


class Watcher(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, "/StatusNotifierWatcher")
        self.items = []

    @dbus.service.method(IFACE, in_signature="s", sender_keyword="sender")
    def RegisterStatusNotifierItem(self, service, sender=None):
        name = service if service.startswith(":") or "." in service else sender
        if name not in self.items:
            self.items.append(name)
        with open(log_path, "a") as f:
            f.write(f"register {name}\n")
        self.StatusNotifierItemRegistered(name)

    @dbus.service.method(IFACE, in_signature="s")
    def RegisterStatusNotifierHost(self, service):
        pass

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature="ss", out_signature="v")
    def Get(self, interface, prop):
        return self.GetAll(interface)[prop]

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return {
            "RegisteredStatusNotifierItems": dbus.Array(self.items, signature="s"),
            "IsStatusNotifierHostRegistered": dbus.Boolean(True),
            "ProtocolVersion": dbus.Int32(0),
        }

    @dbus.service.signal(IFACE, signature="s")
    def StatusNotifierItemRegistered(self, service):
        pass

    @dbus.service.signal(IFACE, signature="s")
    def StatusNotifierItemUnregistered(self, service):
        pass

    @dbus.service.signal(IFACE)
    def StatusNotifierHostRegistered(self):
        pass


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
name = dbus.service.BusName(IFACE, bus)
watcher = Watcher(bus)
GLib.MainLoop().run()
