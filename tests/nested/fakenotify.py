#!/usr/bin/env python3
# Test helper (#93, #98): a stand-in notification server for the nested session's private bus.
# Logs every notification to argv[1] as "summary|body|action keys" and, when a summary starts with
# argv[2], presses its first non-default action after a second.
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

log_path = sys.argv[1]
invoke_prefix = sys.argv[2] if len(sys.argv) > 2 else None


class Server(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, "/org/freedesktop/Notifications")
        self.next_id = 1

    @dbus.service.method("org.freedesktop.Notifications", out_signature="as")
    def GetCapabilities(self):
        return ["actions", "body", "body-markup"]

    @dbus.service.method("org.freedesktop.Notifications", out_signature="ssss")
    def GetServerInformation(self):
        return ["fakenotify", "glimply-tests", "1", "1.2"]

    @dbus.service.method("org.freedesktop.Notifications", in_signature="susssasa{sv}i", out_signature="u")
    def Notify(self, app, replaces, icon, summary, body, actions, hints, timeout):
        nid = replaces or self.next_id
        self.next_id += 1
        keys = [str(actions[i]) for i in range(0, len(actions), 2)]
        with open(log_path, "a") as f:
            f.write(f"{summary}|{body}|{','.join(keys)}\n")
        if invoke_prefix and str(summary).startswith(invoke_prefix):
            key = next((k for k in keys if k != "default"), None)
            if key:
                GLib.timeout_add(1000, lambda: (self.ActionInvoked(nid, key), False)[1])
        return dbus.UInt32(nid)

    @dbus.service.method("org.freedesktop.Notifications", in_signature="u")
    def CloseNotification(self, nid):
        self.NotificationClosed(nid, 3)

    @dbus.service.signal("org.freedesktop.Notifications", signature="us")
    def ActionInvoked(self, nid, key):
        pass

    @dbus.service.signal("org.freedesktop.Notifications", signature="uu")
    def NotificationClosed(self, nid, reason):
        pass


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
name = dbus.service.BusName("org.freedesktop.Notifications", bus)
server = Server(bus)
GLib.MainLoop().run()
