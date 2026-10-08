#!/usr/bin/env python3
"""One step of the packaging smoke test, driven over AT-SPI.

    drive.py <command> [args...]

Runs inside run.sh's private desktop (Xvfb + its own D-Bus session). The
packaged app is sandboxed, so it's found by its AT-SPI name, not its PID.
Window actions go through org.gtk.Actions on the app's bus name, which
needs no keyboard focus. Adapted from tools/demo-tour's tourlib
(agent/strategist-demo).

Environment: SMOKE_OUT (the output folder), SMOKE_RUNNER (flatpak|snap),
SMOKE_APP_ID, SMOKE_SNAP_NAME.
"""
import os
import subprocess
import sys
import time

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402

OUT = os.environ["SMOKE_OUT"]
RUNNER = os.environ.get("SMOKE_RUNNER", "flatpak")
APP_ID = os.environ.get("SMOKE_APP_ID", "com.ustudio.VideoEditor")
SNAP_NAME = os.environ.get("SMOKE_SNAP_NAME", "u-studio-video-editor")
APP = "u-studio-video-editor"  # the AT-SPI application name
HERE = os.path.dirname(os.path.abspath(__file__))
CLICKABLE = ("button", "toggle button", "push button", "menu item", "check menu item", "radio menu item",
             "page tab", "list item", "label", "combo box", "radio button", "table cell")


def log(message):
    with open(os.path.join(OUT, "steps.log"), "a") as f:
        f.write(time.strftime("%H:%M:%S ") + message + "\n")
    print(message)


def app_node():
    desktop = Atspi.get_desktop(0)
    for i in range(desktop.get_child_count()):
        app = desktop.get_child_at_index(i)
        try:
            if app and app.get_name() == APP:
                return app
        except Exception:
            pass
    return None


def walk(node, depth=0):
    if node is None or depth > 60:
        return
    yield node
    try:
        count = node.get_child_count()
    except Exception:
        return
    for i in range(count):
        try:
            yield from walk(node.get_child_at_index(i), depth + 1)
        except Exception:
            pass


def info(node):
    try:
        name = node.get_name() or ""
    except Exception:
        name = ""
    try:
        role = node.get_role_name()
    except Exception:
        role = "?"
    return name, role


def action_names(node):
    iface = node.get_action_iface()
    return [iface.get_action_name(i) for i in range(iface.get_n_actions())] if iface else []


def find(text, roles=CLICKABLE, exact=False, timeout=8):
    end = time.time() + timeout
    while time.time() < end:
        for node in walk(app_node()):
            name, role = info(node)
            if role in roles and (name == text if exact else name.startswith(text)):
                return node
        time.sleep(0.3)
    return None


def press(text, exact=False):
    node = find(text, exact=exact)
    if not node:
        log(f"press '{text}': NOT FOUND")
        return False
    for i, name in enumerate(action_names(node)):
        if name in ("click", "activate", "press", "toggle", "select"):
            node.get_action_iface().do_action(i)
            log(f"press '{text}'")
            return True
    log(f"press '{text}': no action")
    return False


def act(name, param=None):
    result = subprocess.run(["gdbus", "call", "--session", "--dest", APP_ID, "--object-path",
                             "/" + APP_ID.replace(".", "/") + "/window/1", "--method", "org.gtk.Actions.Activate",
                             name, param or "[]", "{}"], capture_output=True, text=True, timeout=30)
    log(f"act {name}: {'ok' if result.returncode == 0 else 'FAILED ' + result.stderr.strip()[:200]}")
    return result.returncode == 0


KEYCODES = {"ctrl": 37, "shift": 50, "alt": 64}


def key(name, down):
    Atspi.generate_keyboard_event(KEYCODES[name], None,
                                  Atspi.KeySynthType.PRESS if down else Atspi.KeySynthType.RELEASE)
    time.sleep(0.08)


def keysym(sym, mods=()):
    for m in mods:
        key(m, True)
    Atspi.generate_keyboard_event(sym, None, Atspi.KeySynthType.SYM)
    time.sleep(0.08)
    for m in reversed(mods):
        key(m, False)
    time.sleep(0.2)


def mouse(x, y, event):
    Atspi.generate_mouse_event(int(x), int(y), event)


def focused_text(timeout=4):
    end = time.time() + timeout
    while time.time() < end:
        for node in walk(app_node()):
            try:
                if node.get_role_name() in ("text", "entry") and \
                        node.get_state_set().contains(Atspi.StateType.FOCUSED):
                    return node
            except Exception:
                pass
        time.sleep(0.2)
    return None


def app_log():
    try:
        return open(os.path.join(OUT, "app.log"), errors="replace").read().splitlines()
    except FileNotFoundError:
        return []


def status():
    for line in reversed(app_log()):
        i = line.find("[app] status: ")
        if i >= 0:
            return line[i + len("[app] status: "):]
    return ""


def launch(env_args):
    """env_args: NAME=value pairs for the app, plus runner flags starting with --."""
    env = dict(os.environ)
    env.update({"USTUDIO_LOG_LEVEL": "debug", "GDK_BACKEND": "x11", "GTK_A11Y": "atspi"})
    # Tags this run's processes, so the checks never inspect another copy of
    # the editor running on the machine.
    pairs = [a for a in env_args if not a.startswith("--")] + [f"USTUDIO_SMOKE_RUN={OUT}"]
    if RUNNER == "flatpak":
        # fallback-x11 withholds X11 while the desktop's wayland-0 socket
        # exists, so the test grants X11 for the private Xvfb explicitly.
        command = ["flatpak", "run", "--user", "--no-documents-portal", "--socket=x11", "--nosocket=wayland"] + \
                  [a for a in env_args if a.startswith("--")] + \
                  [f"--env={p}" for p in pairs + ["USTUDIO_LOG_LEVEL=debug", "GDK_BACKEND=x11", "GTK_A11Y=atspi"]] + \
                  [APP_ID]
    else:
        # snap-confine wants the app in a snap.<name>.<app>-*.scope cgroup.
        # `snap run` asks systemd for one over the session bus, which is this
        # test's private bus, where that fails; from a shell inside another
        # snap's scope (a VS Code terminal) snap-confine then refuses to start
        # (2026-10-08). systemd-run reaches the user manager through
        # $XDG_RUNTIME_DIR/systemd/private and keeps this environment.
        command = ["systemd-run", "--user", "--scope", "--quiet",
                   f"--unit=snap.{SNAP_NAME}.{SNAP_NAME}-{os.getpid()}-{int(time.time())}.scope",
                   "--", "snap", "run", SNAP_NAME]
        env.update(dict(p.split("=", 1) for p in pairs))
    subprocess.Popen(command, env=env, stdout=open(os.path.join(OUT, "app.log"), "a"), stderr=subprocess.STDOUT,
                     stdin=subprocess.DEVNULL, start_new_session=True)
    for _ in range(150):
        if app_node():
            break
        time.sleep(0.2)
    log(f"launch ({RUNNER}): app {'found' if app_node() else 'MISSING'}")
    subprocess.run([sys.executable, os.path.join(HERE, "fitwin.py")], capture_output=True, timeout=30)
    time.sleep(1.5)


def main():
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "launch":
        launch(args)
    elif cmd == "act":
        act(args[0], args[1] if len(args) > 1 else None)
    elif cmd == "press":
        press(args[0], exact=len(args) > 1 and args[1] == "exact")
    elif cmd == "loc":  # Ctrl+L in a GTK file chooser, then set the entry text directly
        keysym(ord("l"), mods=("ctrl",))
        time.sleep(0.5)
        node = focused_text()
        if node is None:
            log("loc: no focused entry")
        else:
            node.get_editable_text_iface().set_text_contents(args[0])
            time.sleep(0.6)
            log(f"loc {args[0]}")
    elif cmd == "settext":  # the focused entry (a save dialog's name field)
        node = focused_text()
        if node is None:
            log("settext: no focused entry")
        else:
            node.get_editable_text_iface().set_text_contents(args[0])
            log(f"settext {args[0]}")
    elif cmd == "enter":
        keysym(0xFF0D)
        log("enter")
    elif cmd == "keysym":  # keysym <X keysym, e.g. 0xff1b for Escape> [ctrl|shift|alt ...]
        keysym(int(args[0], 0), tuple(args[1:]))
        log(f"keysym {args}")
    elif cmd == "click":
        x, y = int(args[0]), int(args[1])
        mouse(x, y, "abs")
        time.sleep(0.2)
        mouse(x, y, "b1c")
        time.sleep(0.3)
        log(f"click {x},{y}")
    elif cmd == "drag":
        x0, y0, x1, y1 = map(int, args[:4])
        mouse(x0, y0, "abs")
        time.sleep(0.3)
        mouse(x0, y0, "b1p")
        time.sleep(0.2)
        for i in range(1, 21):
            mouse(x0 + (x1 - x0) * i / 20, y0 + (y1 - y0) * i / 20, "abs")
            time.sleep(0.03)
        mouse(x1, y1, "b1r")
        time.sleep(0.2)
        log(f"drag {args}")
    elif cmd == "shot":
        path = os.path.join(OUT, args[0] + ".png")
        subprocess.run(["import", "-window", "root", path], capture_output=True, timeout=30)
        log(f"shot {path}")
    elif cmd == "status":
        print(status())
    elif cmd == "rowtext":  # a preferences row's title, description and label texts, on one line
        end = time.time() + 8
        while time.time() < end:
            for node in walk(app_node()):
                name, role = info(node)
                if name == args[0]:
                    try:
                        description = node.get_description() or ""
                    except Exception:
                        description = ""
                    labels = [info(child)[0] for child in walk(node) if info(child)[1] == "label" and info(child)[0]]
                    print(" | ".join([name, description] + labels))
                    return
            time.sleep(0.3)
        print("(row not found)")
    elif cmd == "waitlog":
        end = time.time() + (float(args[1]) if len(args) > 1 else 30)
        found = None
        while time.time() < end and not found:
            found = next((line for line in app_log() if args[0] in line), None)
            if not found:
                time.sleep(0.5)
        log(f"waitlog {args[0]!r}: {found or 'TIMEOUT'}")
    elif cmd == "close":
        for node in walk(app_node()):
            if info(node)[1] == "frame":
                names = action_names(node)
                if "window.close" in names:
                    node.get_action_iface().do_action(names.index("window.close"))
                    log("close: window.close")
                break
    else:
        sys.exit(f"unknown command {cmd}")


if __name__ == "__main__":
    main()
