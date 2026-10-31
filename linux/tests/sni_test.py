#!/usr/bin/env python3
"""Tests the StatusNotifierItem tray icon against a fake tray host.

Run inside a private D-Bus session:
    dbus-run-session -- python3 linux/tests/sni_test.py build/sni_test

It plays the part of a panel (like Ubuntu's AppIndicator extension): it owns
org.kde.StatusNotifierWatcher, accepts the registration, reads the icon,
tooltip and menu, opens the menu, clicks items and the icon, and checks that
the item re-registers when the host restarts or appears late.
"""
import subprocess
import sys
import threading
import time
import queue

from gi.repository import Gio, GLib

WATCHER_XML = """
<node><interface name='org.kde.StatusNotifierWatcher'>
  <method name='RegisterStatusNotifierItem'><arg type='s' direction='in'/></method>
  <method name='RegisterStatusNotifierHost'><arg type='s' direction='in'/></method>
  <property name='RegisteredStatusNotifierItems' type='as' access='read'/>
  <property name='IsStatusNotifierHostRegistered' type='b' access='read'/>
  <property name='ProtocolVersion' type='i' access='read'/>
</interface></node>"""

failures = []


def check(cond, what):
    print(("PASS " if cond else "FAIL ") + what, flush=True)
    if not cond:
        failures.append(what)


class FakeWatcher:
    """A StatusNotifierWatcher running on its own connection and thread."""

    def __init__(self):
        self.items = []
        self.ctx = GLib.MainContext.new()
        self.loop = GLib.MainLoop.new(self.ctx, False)
        self.ready = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()
        self.ready.wait(5)

    def _run(self):
        self.ctx.push_thread_default()
        addr = Gio.dbus_address_get_for_bus_sync(Gio.BusType.SESSION, None)
        self.conn = Gio.DBusConnection.new_for_address_sync(
            addr, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
        node = Gio.DBusNodeInfo.new_for_xml(WATCHER_XML)
        self.reg = self.conn.register_object(
            "/StatusNotifierWatcher", node.interfaces[0], self._method, self._prop, None)
        self.conn.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                            "RequestName", GLib.Variant("(su)", ("org.kde.StatusNotifierWatcher", 4)),
                            None, Gio.DBusCallFlags.NONE, -1, None)
        self.ready.set()
        self.loop.run()
        self.conn.unregister_object(self.reg)
        self.conn.close_sync(None)

    def _method(self, conn, sender, path, iface, method, params, inv):
        if method == "RegisterStatusNotifierItem":
            self.items.append((sender, params.unpack()[0]))
        inv.return_value(None)

    def _prop(self, conn, sender, path, iface, prop):
        return {"RegisteredStatusNotifierItems": GLib.Variant("as", [i[1] for i in self.items]),
                "IsStatusNotifierHostRegistered": GLib.Variant("b", True),
                "ProtocolVersion": GLib.Variant("i", 0)}[prop]

    def stop(self):
        self.ctx.invoke_full(GLib.PRIORITY_DEFAULT, lambda *a: self.loop.quit())
        self.thread.join(5)


class Item:
    def __init__(self, exe):
        self.proc = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.lines = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            self.lines.put(line.strip())

    def expect(self, prefix, timeout=3):
        end = time.time() + timeout
        while time.time() < end:
            try:
                line = self.lines.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                break
            if line.startswith(prefix):
                return line
        return None

    def send(self, cmd):
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()


bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)


def call(name, path, iface, method, args=None, sig=None):
    return bus.call_sync(name, path, iface, method, args, GLib.VariantType(sig) if sig else None,
                         Gio.DBusCallFlags.NONE, 3000, None).unpack()


def get_all(name, path, iface):
    return call(name, path, "org.freedesktop.DBus.Properties", "GetAll", GLib.Variant("(s)", (iface,)))[0]


def find(layout, label):
    if layout[1].get("label") == label:
        return layout
    for child in layout[2]:
        hit = find(child, label)
        if hit:
            return hit
    return None


def main():
    exe = sys.argv[1]

    # 1. Host present at start.
    watcher = FakeWatcher()
    item = Item(exe)
    check(item.expect("REGISTERED") == "REGISTERED 1", "registers with a running host")
    time.sleep(0.2)
    check(len(watcher.items) == 1, "watcher received exactly one registration")
    name = watcher.items[0][1]
    check(name.startswith("org.kde.StatusNotifierItem-"), "registered with its bus name")

    props = get_all(name, "/StatusNotifierItem", "org.kde.StatusNotifierItem")
    check(props["Id"] == "fxsound" and props["Title"] == "FxSound", "Id and Title")
    check(props["Status"] == "Active" and props["ItemIsMenu"] is False, "Status and ItemIsMenu")
    check(props["Menu"] == "/MenuBar", "Menu object path")
    sizes = [(w, h, len(px)) for (w, h, px) in props["IconPixmap"]]
    check(sizes == [(16, 16, 1024), (22, 22, 1936)], "IconPixmap sizes and byte counts")
    check(bytes(props["IconPixmap"][0][2][:4]) == bytes([255, 200, 0, 0]), "IconPixmap is ARGB in network order")
    check(props["ToolTip"][2] == "FxSound" and "Speakers_1" in props["ToolTip"][3], "ToolTip")

    intro = call(name, "/StatusNotifierItem", "org.freedesktop.DBus.Introspectable", "Introspect")[0]
    check("name=\"Activate\"" in intro or "name='Activate'" in intro, "Activate is introspectable")

    menu_props = get_all(name, "/MenuBar", "com.canonical.dbusmenu")
    check(menu_props["Version"] == 3, "dbusmenu Version 3")

    rev, layout = call(name, "/MenuBar", "com.canonical.dbusmenu", "GetLayout",
                       GLib.Variant("(iias)", (0, -1, [])))
    check(layout[0] == 0 and layout[1].get("children-display") == "submenu", "root node")
    labels = [c[1].get("label", c[1].get("type")) for c in layout[2]]
    check(labels == ["Open", "separator", "Preset__Select", "Playback Device", "Exit"], f"top-level items {labels}")
    presets = find(layout, "Preset__Select")
    check(len(presets[2]) == 3 and presets[1].get("children-display") == "submenu", "submenu with 3 presets")
    ticked = [c[1]["label"] for c in presets[2] if c[1].get("toggle-state") == 1]
    check(ticked == ["Preset 1"], "checked preset has a check mark")
    check(find(layout, "Playback Device")[1]["enabled"] is False, "section header disabled")

    # Depth-limited layout.
    _, shallow = call(name, "/MenuBar", "com.canonical.dbusmenu", "GetLayout", GLib.Variant("(iias)", (0, 0, [])))
    check(len(shallow[2]) == 0, "recursionDepth 0 returns no children")
    _, only = call(name, "/MenuBar", "com.canonical.dbusmenu", "GetLayout", GLib.Variant("(iias)", (0, -1, ["label"])))
    check(set(only[2][0][1].keys()) == {"label"}, "propertyNames filter")

    group = call(name, "/MenuBar", "com.canonical.dbusmenu", "GetGroupProperties",
                 GLib.Variant("(aias)", ([layout[2][0][0]], [])))[0]
    check(group[0][1]["label"] == "Open", "GetGroupProperties")
    prop = call(name, "/MenuBar", "com.canonical.dbusmenu", "GetProperty",
                GLib.Variant("(is)", (layout[2][0][0], "label")))[0]
    check(prop == "Open", "GetProperty")

    # Click a menu item, a submenu item, and a disabled one.
    def click(node_id):
        call(name, "/MenuBar", "com.canonical.dbusmenu", "Event",
             GLib.Variant("(isvu)", (node_id, "clicked", GLib.Variant("i", 0), 0)))
    click(find(layout, "Exit")[0])
    check(item.expect("ACTION") == "ACTION 5", "clicking Exit reports its action")
    click(find(layout, "Preset 2")[0])
    check(item.expect("ACTION") == "ACTION 102", "clicking a submenu item reports its action")
    click(find(layout, "Playback Device")[0])
    check(item.expect("ACTION", 0.5) is None, "disabled item does nothing")

    # Activate (left click on the icon).
    call(name, "/StatusNotifierItem", "org.kde.StatusNotifierItem", "Activate", GLib.Variant("(ii)", (100, 5)))
    check(item.expect("ACTIVATE") == "ACTIVATE 100 5", "Activate reaches the app")

    # AboutToShow: the app sends a fresh menu (one more preset) -> needUpdate and a new revision.
    signals = []
    sub = bus.signal_subscribe(name, "com.canonical.dbusmenu", "LayoutUpdated", "/MenuBar", None,
                               Gio.DBusSignalFlags.NONE, lambda *a: signals.append(a[5].unpack()))
    need = call(name, "/MenuBar", "com.canonical.dbusmenu", "AboutToShow", GLib.Variant("(i)", (0,)))[0]
    check(item.expect("ABOUT") is not None, "AboutToShow asks the app for fresh items")
    check(need is True, "AboutToShow reports the menu changed")
    rev2, layout2 = call(name, "/MenuBar", "com.canonical.dbusmenu", "GetLayout", GLib.Variant("(iias)", (0, -1, [])))
    check(rev2 > rev and len(find(layout2, "Preset__Select")[2]) == 4, "new revision with the fresh items")
    ctx = GLib.MainContext.default()
    end = time.time() + 1
    while time.time() < end and not signals:
        ctx.iteration(False)
        time.sleep(0.01)
    check(len(signals) >= 1 and signals[0][0] == rev2, "LayoutUpdated signal with the new revision")

    # Same items again -> no new revision.
    item.send("menu 1")
    item.expect("OK menu 1")
    time.sleep(0.1)
    rev3, _ = call(name, "/MenuBar", "com.canonical.dbusmenu", "GetLayout", GLib.Variant("(iias)", (0, -1, [])))
    check(rev3 == rev2, "unchanged menu keeps its revision")

    # Icon change emits NewIcon and updates IconPixmap.
    icon_signals = []
    bus.signal_subscribe(name, "org.kde.StatusNotifierItem", "NewIcon", "/StatusNotifierItem", None,
                         Gio.DBusSignalFlags.NONE, lambda *a: icon_signals.append(1))
    item.send("icon")
    item.expect("OK icon")
    end = time.time() + 1
    while time.time() < end and not icon_signals:
        ctx.iteration(False)
        time.sleep(0.01)
    props = get_all(name, "/StatusNotifierItem", "org.kde.StatusNotifierItem")
    check(icon_signals and bytes(props["IconPixmap"][0][2][:4]) == bytes([255, 10, 0, 0]), "NewIcon + updated pixmap")
    bus.signal_unsubscribe(sub)

    # 2. Host restarts (e.g. GNOME Shell restarted): item registers again.
    watcher.stop()
    check(item.expect("HOST") == "HOST 0", "notices the host going away")
    watcher = FakeWatcher()
    check(item.expect("HOST") == "HOST 1", "re-registers when the host comes back")
    time.sleep(0.2)
    check(len(watcher.items) == 1, "new host got the registration")

    item.send("quit")
    check(item.expect("EXITED") == "EXITED", "shuts down cleanly")
    item.proc.wait(5)
    check(item.proc.returncode == 0, "exit code 0")
    watcher.stop()

    # 3. No host at start (stock GNOME without the AppIndicator extension).
    item = Item(exe)
    t0 = time.time()
    check(item.expect("REGISTERED") == "REGISTERED 0", "reports no host")
    check(time.time() - t0 < 1.5, "does not wait the full timeout when there is clearly no host")
    watcher = FakeWatcher()   # extension enabled later
    check(item.expect("HOST") == "HOST 1", "registers when a host appears later")
    item.send("quit")
    check(item.expect("EXITED") == "EXITED", "shuts down cleanly (late host)")
    watcher.stop()

    print(f"\n{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
