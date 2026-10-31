#!/usr/bin/env python3
"""End-to-end tray test of the real FxSound app.

    dbus-run-session -- python3 linux/tests/tray_app_test.py build/FxSound_artefacts/Release/FxSound

Starts a private PipeWire (with a null "speakers" sink), an Xvfb display, a
fake XEmbed system tray (counts dock requests) and, per scenario, a fake
StatusNotifierWatcher (from sni_test.py). Checks which tray FxSound uses on
GNOME and elsewhere, drives the D-Bus menu, and checks that the XEmbed
fallback docks its window only once (re-docking crashed GNOME Shell).
Needs: pipewire, wireplumber, Xvfb, xdotool, python3-gi, python-xlib.
"""
import json
import os
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sni_test import FakeWatcher, check, failures, call, get_all, find  # noqa: E402
from gi.repository import GLib  # noqa: E402
from Xlib import X, display as xdisplay  # noqa: E402

APP = os.path.abspath(sys.argv[1])
DISPLAY = ":97"


class FakeXEmbedTray:
    """Owns _NET_SYSTEM_TRAY_S0 and counts SYSTEM_TRAY_REQUEST_DOCK messages."""

    def __init__(self):
        self.docks = []
        self.d = xdisplay.Display(DISPLAY)
        screen = self.d.screen()
        self.win = screen.root.create_window(0, 0, 1, 1, 0, screen.root_depth)
        self.sel = self.d.intern_atom("_NET_SYSTEM_TRAY_S0")
        self.opcode = self.d.intern_atom("_NET_SYSTEM_TRAY_OPCODE")
        self.win.set_selection_owner(self.sel, X.CurrentTime)
        self.d.flush()
        self.stop_flag = False
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while not self.stop_flag:
            while self.d.pending_events():
                ev = self.d.next_event()
                if ev.type == X.ClientMessage and ev.client_type == self.opcode:
                    _, data = ev.data
                    if data[1] == 0:
                        self.docks.append(data[2])
            time.sleep(0.02)


def start_env(tmp):
    env = dict(os.environ)
    env.update(HOME=f"{tmp}/home", XDG_RUNTIME_DIR=f"{tmp}/run", XDG_CONFIG_HOME=f"{tmp}/home/.config",
               DISPLAY=DISPLAY)
    os.makedirs(env["HOME"], exist_ok=True)
    os.makedirs(env["XDG_RUNTIME_DIR"], mode=0o700, exist_ok=True)
    procs = [subprocess.Popen(["Xvfb", DISPLAY, "-screen", "0", "1280x800x24", "-nolisten", "tcp"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
             subprocess.Popen(["pipewire"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)]
    time.sleep(1)
    procs.append(subprocess.Popen(["wireplumber"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
    time.sleep(1.5)
    subprocess.run(["pw-cli", "create-node", "adapter",
                    "{ factory.name=support.null-audio-sink node.name=speakers node.description=Speakers "
                    "media.class=Audio/Sink audio.position=[FL FR] object.linger=true }"],
                   env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    return env, procs


class App:
    def __init__(self, env, desktop, tray=None):
        e = dict(env, XDG_CURRENT_DESKTOP=desktop)
        if tray:
            e["FXSOUND_TRAY"] = tray
        self.env = e
        self.proc = subprocess.Popen([APP], env=e, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(4)

    def cli(self, *args):
        return subprocess.run([APP, *args], env=self.env, capture_output=True, text=True, timeout=15).stdout

    def status(self):
        out = self.cli("--status")
        try:
            return json.loads(out[out.index("{"):])
        except ValueError:
            return {}

    def window_visible(self):
        r = subprocess.run(["xdotool", "search", "--onlyvisible", "--name", "^FxSound$"],
                           env=self.env, capture_output=True, text=True)
        return bool(r.stdout.strip())

    def alive(self):
        return self.proc.poll() is None

    def quit(self):
        if self.alive():
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        time.sleep(1)


def wait_for(cond, timeout=5):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.1)
    return False


def main():
    tmp = tempfile.mkdtemp(prefix="fxtray")
    env, procs = start_env(tmp)
    tray = FakeXEmbedTray()
    try:
        # --- A: Ubuntu GNOME with the AppIndicator extension (SNI host) ---
        print("== GNOME, AppIndicator host present")
        watcher = FakeWatcher()
        app = App(env, "ubuntu:GNOME")
        check(app.alive(), "app is running")
        check(wait_for(lambda: len(watcher.items) == 1), "registered a StatusNotifierItem")
        check(tray.docks == [], "no XEmbed dock request on GNOME")
        name = watcher.items[0][1] if watcher.items else ""

        props = get_all(name, "/StatusNotifierItem", "org.kde.StatusNotifierItem")
        sizes = sorted(w for (w, h, px) in props["IconPixmap"])
        check(sizes == [16, 22, 24, 32, 48, 64], f"icon in 6 sizes {sizes}")
        check("FxSound is on." in props["ToolTip"][3], "tooltip says FxSound is on")
        icon_on = bytes(props["IconPixmap"][-1][2])

        need = call(name, "/MenuBar", "com.canonical.dbusmenu", "AboutToShow", GLib.Variant("(i)", (0,)))
        _, layout = call(name, "/MenuBar", "com.canonical.dbusmenu", "GetLayout", GLib.Variant("(iias)", (0, -1, [])))
        labels = [c[1].get("label", c[1].get("type")) for c in layout[2]]
        print("   menu:", labels)
        for want in ("Open", "Turn Off", "Preset Select", "Settings", "Theme", "Always On Top", "Exit"):
            check(want in labels, f"menu has '{want}'")
        presets = find(layout, "Preset Select")
        check(presets is not None and len(presets[2]) > 5, "preset submenu filled")
        check(any(c[1].get("toggle-state") == 1 for c in presets[2]), "selected preset is checked")
        check(find(layout, "Speakers") is not None, "output device listed")

        def click(label):
            node = find(layout, label)
            call(name, "/MenuBar", "com.canonical.dbusmenu", "Event",
                 GLib.Variant("(isvu)", (node[0], "clicked", GLib.Variant("i", 0), 0)))

        click("Turn Off")
        check(wait_for(lambda: app.status().get("power") in (False, 0, "off")), "menu 'Turn Off' turns power off")
        props = get_all(name, "/StatusNotifierItem", "org.kde.StatusNotifierItem")
        check(bytes(props["IconPixmap"][-1][2]) != icon_on, "icon changes when off")
        check("FxSound is off." in props["ToolTip"][3], "tooltip says FxSound is off")
        call(name, "/MenuBar", "com.canonical.dbusmenu", "AboutToShow", GLib.Variant("(i)", (0,)))
        _, layout = call(name, "/MenuBar", "com.canonical.dbusmenu", "GetLayout", GLib.Variant("(iias)", (0, -1, [])))
        check(find(layout, "Turn On") is not None and find(layout, "Preset Select") is None, "menu updated for power off")
        click("Turn On")
        check(wait_for(lambda: app.status().get("power") in (True, 1, "on")), "menu 'Turn On' turns power on")

        was_visible = app.window_visible()
        call(name, "/StatusNotifierItem", "org.kde.StatusNotifierItem", "Activate", GLib.Variant("(ii)", (0, 0)))
        check(wait_for(lambda: app.window_visible() != was_visible), "clicking the icon toggles the window")
        call(name, "/StatusNotifierItem", "org.kde.StatusNotifierItem", "Activate", GLib.Variant("(ii)", (0, 0)))
        check(wait_for(lambda: app.window_visible() == was_visible), "clicking again toggles it back")

        for _ in range(3):   # status changes that used to re-dock the XEmbed window
            app.cli("--power=0"); time.sleep(0.5); app.cli("--power=1"); time.sleep(0.5)
        check(tray.docks == [] and app.alive(), "power toggles: still no XEmbed, app alive")

        _, layout = call(name, "/MenuBar", "com.canonical.dbusmenu", "GetLayout", GLib.Variant("(iias)", (0, -1, [])))
        click("Exit")
        check(wait_for(lambda: not app.alive(), 10), "menu 'Exit' quits the app")
        check(app.proc.returncode == 0, f"clean exit code ({app.proc.returncode})")
        watcher.stop()
        time.sleep(1)

        # --- B: stock GNOME, no AppIndicator extension ---
        print("== GNOME, no tray host")
        app = App(env, "ubuntu:GNOME")
        for _ in range(2):
            app.cli("--power=0"); time.sleep(0.5); app.cli("--power=1"); time.sleep(0.5)
        check(app.alive() and tray.docks == [], "runs with no tray icon and no XEmbed")
        watcher = FakeWatcher()   # user enables the extension
        check(wait_for(lambda: len(watcher.items) == 1), "icon appears once a host appears")
        app.quit()
        watcher.stop()
        time.sleep(1)

        # --- C: desktop without SNI host (XEmbed fallback) ---
        print("== XFCE, no SNI host: XEmbed fallback")
        app = App(env, "XFCE")
        for _ in range(3):
            app.cli("--power=0"); time.sleep(0.5); app.cli("--power=1"); time.sleep(0.5)
        check(app.alive(), "app is running")
        check(len(tray.docks) == 1, f"XEmbed icon docked exactly once ({len(tray.docks)} requests)")
        app.quit()
        tray.docks.clear()

        # --- D: KDE-like desktop with SNI host ---
        print("== KDE, SNI host present")
        watcher = FakeWatcher()
        app = App(env, "KDE")
        check(wait_for(lambda: len(watcher.items) == 1) and tray.docks == [], "uses SNI, not XEmbed")
        app.quit()
        watcher.stop()

        # --- E: overrides ---
        print("== FXSOUND_TRAY=none on XFCE")
        app = App(env, "XFCE", tray="none")
        check(app.alive() and tray.docks == [], "no tray icon at all")
        app.quit()
    finally:
        tray.stop_flag = True
        for p in reversed(procs):
            p.terminate()
    print(f"\n{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
