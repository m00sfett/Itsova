#!/usr/bin/env python3
# Test helper (#93): full-workspace screenshot of a nested KWin via org.kde.KWin.ScreenShot2.
# Only works where a desktop file grants this python binary the restricted interface (nested
# session with a throw-away HOME, set up by tests/nested/session.sh). Usage: kwinshot.py <out.png>
import os, subprocess, sys, threading
import dbus

out = sys.argv[1]
bus = dbus.SessionBus()
shot = dbus.Interface(bus.get_object("org.kde.KWin", "/org/kde/KWin/ScreenShot2"), "org.kde.KWin.ScreenShot2")
r, w = os.pipe()
data = bytearray()
def reader():
    while True:
        chunk = os.read(r, 1 << 20)
        if not chunk:
            break
        data.extend(chunk)
t = threading.Thread(target=reader)
t.start()
meta = shot.CaptureWorkspace({"native-resolution": dbus.Boolean(True)}, dbus.types.UnixFd(w))
os.close(w)
t.join()
os.close(r)
width, height, stride = int(meta["width"]), int(meta["height"]), int(meta["stride"])
rows = b"".join(bytes(data[y * stride:y * stride + width * 4]) for y in range(height))
fmt = int(meta.get("format", 6))
layout = "rgba" if fmt in (16, 17, 18) else "bgra"  # QImage::Format_RGBX/RGBA8888 vs ARGB32
subprocess.run(["magick", "-size", f"{width}x{height}", "-depth", "8", f"{layout}:-", out], input=rows, check=True)
print(out, width, height, "format", int(meta.get("format", -1)))
