#!/usr/bin/env python3
"""Yoyu tray — the companion as a menubar / system-tray app.

Sits quietly in your menubar (macOS) or system tray (Windows/Linux): it finds
your Yoyu board, feeds it your Claude usage, and gives you one-click
**Pair** (make the board self-contained) and **Open board page**. The icon's
colour tells you at a glance whether it's feeding (green), searching (amber), or
stuck (red).

All the Claude-usage logic is reused from companion.py — this file is just the
tray shell.

Run from source:
    pip install pystray pillow certifi        # + pyobjc-framework-Cocoa on macOS
    python tray.py
"""

import os
import sys
import threading
import time
import webbrowser

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import companion  # noqa: E402  (reuse discover/pair/feed logic)

try:
    import pystray
    from PIL import Image, ImageDraw
except ImportError:
    sys.stderr.write(
        "The tray app needs two small libraries. Install them and re-run:\n"
        "  pip install pystray pillow certifi"
        + ("  pyobjc-framework-Cocoa\n" if sys.platform == "darwin" else "\n"))
    sys.exit(1)

INTERVAL = 120  # seconds between feeds

# Shared state. Python's GIL makes these simple dict updates safe enough here.
# url may be pinned up front via the HEADROOM_PI env var or a saved config, so
# a fussy network (VPNs, work laptops) doesn't have to be auto-discovered.
# "url" is the first board and stays the one that single-target actions use;
# "boards" is all of them. Two boards on one desk is now an ordinary setup
# rather than a curiosity, and feeding only the first meant the second sat
# there saying it was waiting for this computer.
state = {"color": "amber", "status": "Starting…", "url": None, "boards": [],
         "swept": None, "feeding": True, "fixed": False, "avatar": None}


def initial_urls():
    u = os.environ.get("HEADROOM_PI", "").strip()
    if not u:
        try:
            u = companion.load_config().get("pi") or ""
        except Exception:  # noqa: BLE001
            u = ""
    return [t.strip().rstrip("/") for t in str(u).split(",") if t.strip()]


def _host(url):
    return url.split("//")[-1].split(":")[0]


def board_label(b):
    """What to call a board in a menu. The id once the firmware offers one,
    since addresses move and mDNS names are handed out in boot order."""
    return "%s · %s" % (b.get("board") or "board", b.get("id") or _host(b["url"]))


def set_boards(icon, boards):
    """Adopt a new board list and rebuild the menu around it.

    The per-board submenus are built from this list, so they have to be rebuilt
    rather than merely refreshed when it changes.
    """
    state["boards"] = boards
    state["url"] = boards[0]["url"] if boards else None
    if icon is not None:
        try:
            icon.menu = build_menu()
        except Exception:  # noqa: BLE001 - a menu rebuild is never worth dying for
            pass


def enrich_boards(icon):
    """Put real names on boards restored from the config.

    They come back from disk as bare addresses, which would leave the menu
    offering two entries both called "board". One status call each, off the
    main thread, and only for the ones we don't already know.
    """
    changed = False
    for b in state["boards"]:
        if b.get("id") or b.get("board") not in (None, "board"):
            continue
        info = companion._probe_info(b["url"], timeout=3)
        if info:
            b["id"] = info.get("id")
            b["board"] = info.get("board") or "board"
            b["version"] = info.get("version") or "?"
            changed = True
        # The first board's character drives the tray icon. With two boards the
        # icon can only be one of them, and the first is the one every other
        # single-target action already uses.
        if b is state["boards"][0]:
            info = info or companion._probe_info(b["url"], timeout=3)
            if info and info.get("avatar") != state.get("avatar"):
                state["avatar"] = info.get("avatar")
                changed = True
    if changed and icon is not None:
        try:
            icon.menu = build_menu()
        except Exception:  # noqa: BLE001
            pass


def discover_boards(icon):
    found = companion.discover_all(quiet=True)
    if found:
        companion.save_pi(",".join(b["url"] for b in found))
        set_boards(icon, found)
    return found

COLORS = {"green": (94, 170, 100), "amber": (230, 164, 23),
          "red": (221, 77, 77), "grey": (140, 140, 140)}


# The kitsune's head — the same cells the board draws, cropped to the head and
# ruff. The full sprite carries a fan of tails that says how much headroom is
# left, which is the board's job; a 64px tray icon has room for the animal or
# the gauge, not both. The ear interiors are tinted by feed state (green
# feeding / amber busy / red stuck) so the icon still reports status at a glance.
KITSUNE_HEAD = [".K.......K.", ".KSK...KSK.", "KBSSK.KSSBK", "KBBBKKKBBBK",
                "KBBBBBBBBBK", "KBBBBBBBBBK", ".KBBBBBBBK.", "..KBWWWBK..",
                "...KWWWK...", ".KBBBBBBBK.", ".KBBWWWBBK.", ".KBBWWWBBK.",
                "..KK...KK.."]
_SPRK = {"K": (26, 24, 22), "W": (250, 247, 239),
         "B": (201, 96, 63), "S": (158, 68, 41)}

# The other four characters, cropped to what survives at 11x13 cells. A tray
# icon is about 22 real pixels across on a HiDPI bar, so these are the
# recognisable part of each -- the moon's disc, the candle's flame and stub,
# the plant's pot and shoot, the cat's head -- not a shrunken copy of the
# board's sprite, which turns to mush at this size.
#
# The GAUGE is deliberately left on the board. The icon has room for the
# character or the measurement, not both, and the measurement is the board's
# entire job; the icon's job is to say which board this is and whether the
# feed is healthy.
_LIT, _DARK = (232, 228, 208), (74, 72, 62)
_WAX, _WAXD = (232, 224, 200), (184, 174, 147)
_LEAF, _LEAFD = (122, 168, 92), (78, 111, 58)

MOON_HEAD = ["...LLLLL...", ".LLLLLLLLL.", ".LLLLLLLLL.", "LLLLLLLLLLL",
             "LLLLLLLLLLL", "LLLLLLLLLLL", "LLLLLLLLLLL", "LLLLLLLLLLL",
             "LLLLLLLLLLL", ".LLLLLLLLL.", ".LLLLLLLLL.", "...LLLLL...",
             "..........."]
CANDLE_HEAD = ["....F......", "....F......", "...KKK.....", "...WWD.....",
               "...WWD.....", "...WWD.....", "...WWD.....", "...WWD.....",
               "...WWD.....", "..DDDDD....", ".DDDDDDD...", "...........",
               "..........."]
PLANT_HEAD = [".....G.....", "...GGG.GG..", "..GG.G.....", ".....G.GG..",
              "...GG.G....", ".....G.....", "..ggggggg..", ".BBBBBBBBB.",
              ".BBBBBBBBB.", "..BBBBBBB..", "..BBBBBBB..", "...BBBBB...",
              "..........."]
CAT_HEAD = ["..K.....K..", ".KSK...KSK.", ".KBSK.KSBK.", ".KBBKKKBBK.",
            "KBBBBBBBBBK", "KBBBBBBBBBK", "KBBBBBBBBBK", ".KBBBBBBBK.",
            ".KBBBBBBBK.", "KBBBBBBBBBK", ".KKKKKKKKK.", "...........",
            "..........."]

_AV_COLS = dict(_SPRK)
_AV_COLS.update({"L": _LIT, "D": _WAXD, "W": _WAX, "F": (250, 178, 25),
                 "G": _LEAF, "g": _LEAFD})

# Keyed by what /api/status reports, so the icon follows the board.
AVATAR_ART = {"Kitsune": KITSUNE_HEAD, "Moon": MOON_HEAD,
              "Candle": CANDLE_HEAD, "Plant": PLANT_HEAD, "Cat": CAT_HEAD}


def make_icon(color, avatar=None):
    """Draw whichever character the board wears, tinted by feed state.

    The status colour has to land somewhere legible on every one of them, and
    they do not share a feature: the kitsune has ears, the moon has neither
    ears nor anything else at the top. So each names its own spot, and the
    fallback is a corner pip -- better a small dot in a known place than a
    tint smeared over a face.
    """
    art = AVATAR_ART.get(avatar or "Kitsune", KITSUNE_HEAD)
    img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([2, 2, 61, 61], radius=14, fill=(38, 38, 36, 255))
    status = COLORS.get(color, COLORS["grey"])
    U = 4                       # cell size; 11x13 cells centred in 64
    padx, pady = (64 - 11 * U) // 2, (64 - 13 * U) // 2

    def cell(cx, cy, rgb):
        x0, y0 = padx + cx * U, pady + cy * U
        d.rectangle([x0, y0, x0 + U - 1, y0 + U - 1], fill=rgb)

    for y, row in enumerate(art):
        for x, ch in enumerate(row):
            col = _AV_COLS.get(ch)
            if col:
                cell(x, y, col)

    if art is KITSUNE_HEAD:
        for x in (2, 8):                                      # ear interiors
            cell(x, 1, status)
        for x in (2, 7):                                      # eyes
            cell(x, 4, _SPRK["K"])
        cell(5, 7, _SPRK["K"])
        cell(4, 8, _SPRK["K"]); cell(6, 8, _SPRK["K"])
    elif art is CAT_HEAD:
        # Its own placement, not the kitsune's. The two sprites are different
        # shapes -- the cat's face sits a row lower and its eyes further in --
        # and borrowing the fox's coordinates left marks in the wrong places.
        for x in (2, 8):                                      # ear interiors
            cell(x, 1, status)
        for x in (3, 7):                                      # eyes
            cell(x, 5, _SPRK["K"])
        cell(5, 6, (217, 119, 87))                            # nose
    elif art is MOON_HEAD:
        for x in (3, 7):                                      # eyes
            cell(x, 5, _SPRK["K"])
        cell(4, 8, _SPRK["K"]); cell(5, 8, _SPRK["K"]); cell(6, 8, _SPRK["K"])
        cell(9, 2, status)                                    # a lit limb
        cell(1, 2, status)
    elif art is CANDLE_HEAD:
        cell(4, 0, status); cell(4, 1, status)                # the flame itself
    else:                                                     # plant
        cell(5, 0, status)                                    # the growing tip
        for x in (4, 6):
            cell(x, 9, _SPRK["K"])                            # eyes on the pot
    return img


def feed_once(urls):
    """One poll, pushed to every board. Returns (color, status, rate_limited).

    One read of Anthropic feeds all of them: the usage is the same whoever is
    displaying it, and polling once per board would multiply the rate limiting
    that already bites here.
    """
    try:
        live = companion.get_live_windows()
    except companion.LiveUnavailable as exc:
        if getattr(exc, "rate_limited", False):
            return "amber", "Rate limited by Anthropic — backing off", True
        return "amber", "Usage temporarily unreadable", False
    if not live:
        # "No Claude login" is true and useless: it does not say what to do, and
        # to somebody whose Claude Code is working in the next window it reads
        # as simply wrong. The tray has one short line, so spend it on the fix.
        st = companion.login_state()
        if st == "not_installed":
            return "red", "Claude Code isn't installed here — click for help", False
        if st == "signed_out":
            return "red", "Claude sign-in cleared — run: claude, then /login", False
        return "red", "Claude Code not signed in — run: claude, then /login", False
    windows, plan, credits = live
    payload = {"windows": windows, "plan": plan, "source": "live"}
    if credits is not None:
        payload["credits"] = credits
    urls = [urls] if isinstance(urls, str) else list(urls)
    delivered, rejected = 0, None
    for url in urls:
        # Keeps a self-hosted board going while this computer is on; skipped
        # for any board this computer never paired with.
        try:
            companion.maybe_top_up(url)
        except Exception:  # noqa: BLE001
            pass
        try:
            res = companion.push(url, "", payload)
        except Exception:  # noqa: BLE001 - network error means "unreachable"
            continue
        if res.get("ok"):
            delivered += 1
        else:
            rejected = str(res.get("error"))
    if delivered == 0:
        if rejected:
            return "amber", "Board rejected: " + rejected, False
        return "red", ("Can't reach the board" if len(urls) < 2
                       else "Can't reach any of your %d boards" % len(urls)), False
    summary = ", ".join(f"{w['label'].split(' (')[0]} {w['utilization']:.0f}%"
                        for w in windows[:3])
    # Only counted out loud when there is more than one, so the ordinary
    # single-board line stays exactly as short as it was.
    where = "" if len(urls) < 2 else " %d/%d ·" % (delivered, len(urls))
    return "green", "Feeding" + where + " · " + summary, False


def refresh(icon):
    icon.icon = make_icon(state["color"], state.get("avatar"))
    icon.update_menu()


def worker(icon):
    rl_backoff = 0   # extra seconds added while Anthropic is rate-limiting us
    while True:
        if not state["feeding"]:
            state.update(color="grey", status="Paused")
            refresh(icon)
            time.sleep(2)
            continue
        if not state["boards"]:
            state.update(color="amber", status="Looking for your boards…")
            refresh(icon)
            if not discover_boards(icon):
                state.update(color="red", status="No board found on this network")
                refresh(icon)
                time.sleep(15)
                continue
        enrich_boards(icon)
        urls = [b["url"] for b in state["boards"]]
        color, status, rate_limited = feed_once(urls)
        if color == "green":
            ensure_autostart()                         # persist a working setup
        elif color == "red" and "reach" in status and not state["fixed"]:
            set_boards(icon, [])                       # lost them -> rediscover
        # Rate-limited: back off exponentially (cap 30 min) so we stop pounding
        # the usage endpoint. A good read snaps us back to the normal cadence.
        if rate_limited:
            rl_backoff = min(1800, rl_backoff * 2 or INTERVAL)
            status = f"{status} (next try ~{(INTERVAL + rl_backoff) // 60}m)"
        else:
            rl_backoff = 0
        state.update(color=color, status=status)
        refresh(icon)
        time.sleep(INTERVAL + rl_backoff)


# ---------------------------------------------------------------- menu actions

def _ask_pair_code():
    """Pop a small dialog for the code the board shows during pairing. The tray
    has no console, so companion.pair_device() can't fall back to input()."""
    try:
        import tkinter as tk
        from tkinter import simpledialog
        root = tk.Tk()
        root.withdraw()
        root.attributes("-topmost", True)
        code = simpledialog.askstring(
            "Pair board",
            "Look at your Yoyu board — it's showing a 6-character code.\n"
            "Enter it here to finish pairing:")
        root.destroy()
        return code or ""
    except Exception:
        return ""


def pair_board(url=None):
    """Pair one specific board. With two on the desk, which one is not a
    detail the tray can guess -- each holds its own key."""
    def _cb(icon, item):
        def run():
            target = url or state["url"] or companion.discover_pi()
            if not target:
                state["status"] = "Pair failed: board not found"
            else:
                ok = companion.pair_device(target, ask_code=_ask_pair_code)
                state["status"] = ("Paired — that board runs on its own now"
                                   if ok else "Pair failed (wrong code, or no "
                                   "Claude login here?)")
            icon.update_menu()
        threading.Thread(target=run, daemon=True).start()
    return _cb


def disconnect_board(url=None):
    """Undo pairing from the same menu that offers it.

    Pairing is one click here; unpairing meant finding a page on the board and
    a grey button at the bottom of it. Confirmed first -- it clears a login.
    """
    def _cb(icon, item):
        def run():
            target = url or state["url"]
            if not target:
                state["status"] = "No board to disconnect"
                icon.update_menu()
                return
            msg = ("Disconnect this board from your Claude account?\n"
                   "Wi-Fi, theme, avatar and history are kept.")
            if not _confirm(msg):
                return
            ok = companion.disconnect_board(target)
            state["status"] = ("Disconnected - it no longer holds a login"
                               if ok else "Disconnect failed")
            icon.update_menu()
        threading.Thread(target=run, daemon=True).start()
    return _cb


def _confirm(msg):
    try:
        import tkinter as tk
        from tkinter import messagebox
        root = tk.Tk(); root.withdraw(); root.attributes("-topmost", True)
        ans = messagebox.askyesno("Yoyu", msg)
        root.destroy()
        return bool(ans)
    except Exception:  # noqa: BLE001 - no GUI toolkit: do not act unasked
        return False


def do_rescan(icon, item):
    """Look again. Nothing rediscovers on its own once a saved board answers,
    which is right until the day you plug in a second one."""
    def run():
        state["status"] = "Looking for boards…"
        icon.update_menu()
        found = discover_boards(icon)
        state["status"] = ("Found %d board%s" % (len(found),
                                                 "" if len(found) == 1 else "s")
                           if found else "No board found on this network")
        icon.update_menu()
    threading.Thread(target=run, daemon=True).start()


def open_path(path, url=None):
    """A menu callback that opens one of the board's config pages in a browser.
    Rich forms (screen toggles, timezone, alert thresholds) live on the board so
    they work even when this computer is off — the tray just deep-links to them."""
    def _cb(icon, item):
        target = url or state["url"]
        if target:
            webbrowser.open(target.rstrip("/") + path)
    return _cb


def do_open(icon, item):
    if state["url"]:
        webbrowser.open(state["url"])


def _board_menu(url):
    return pystray.Menu(
        pystray.MenuItem("Open its page", open_path("", url)),
        pystray.MenuItem("Screens, clock & auto-rotate…", open_path("/settings", url)),
        pystray.MenuItem("Phone alerts…", open_path("/alerts", url)),
        pystray.MenuItem("Update firmware…", open_path("/update", url)),
        pystray.Menu.SEPARATOR,
        pystray.MenuItem("Pair this board", pair_board(url)),
        pystray.MenuItem("Disconnect from Claude", disconnect_board(url)),
    )


def toggle_feeding(icon, item):
    state["feeding"] = not state["feeding"]


def is_autostarted():
    return os.path.isfile(companion.INSTALLED_MARKER)


def ensure_autostart():
    """Deliberately does nothing now, and is kept as the place that explains why.

    This used to add a login item after the first good feed, quietly. Two
    things were wrong with that. Deciding that a program should start with
    somebody's computer is theirs to decide, and the path it recorded was
    wherever the binary happened to be running from, which for a file
    downloaded and double-clicked is the Downloads folder. Emptying Downloads
    then broke start-up with nothing on screen to say so.

    Installing is now something you ask for: the menu offers it, and the CLI
    has --install.
    """
    return


def do_install(icon, item):
    """Copy the app somewhere permanent and offer to start it at login."""
    def run():
        if not getattr(sys, "frozen", False):
            state["status"] = "Running from source: use --install on the binary"
            icon.update_menu()
            return
        startup = _confirm(
            "Install %s and start it when you log in?" % companion.APP_NAME
            + chr(10) +
            "Choosing No still installs it, just without starting at login.")
        try:
            lines = companion.install_app(startup=startup)
        except Exception as exc:  # noqa: BLE001
            state["status"] = "Install failed: %s" % exc
            icon.update_menu()
            return
        with open(companion.INSTALLED_MARKER, "w", encoding="utf-8") as fh:
            fh.write(state.get("url") or "")
        state["status"] = ("Installed" + (" - starts at login" if startup else ""))
        try:
            icon.menu = build_menu()
        except Exception:  # noqa: BLE001
            pass
        icon.update_menu()
        try:
            icon.notify(chr(10).join(lines[:3]), companion.APP_NAME)
        except Exception:  # noqa: BLE001 - not every backend has notify()
            pass
    threading.Thread(target=run, daemon=True).start()


def toggle_autostart(icon, item):
    try:
        if is_autostarted():
            companion.uninstall_autostart()
        else:
            companion.install_autostart()
            with open(companion.INSTALLED_MARKER, "w", encoding="utf-8") as fh:
                fh.write(state.get("url") or "")
    except Exception:  # noqa: BLE001
        pass


HELP_URL = ("https://github.com/DaveEuson/Yoyu/blob/main/docs/"
            "TROUBLESHOOTING.md#the-companion-says-it-cant-find-your-claude-login")


def needs_login():
    return companion.login_state() != "ok"


def do_login_help(icon=None, item=None):
    """Open the written steps. The tray has no room for four numbered lines,
    and a notification is gone before anyone has finished reading it."""
    webbrowser.open(HELP_URL)


def build_menu():
    return pystray.Menu(
        pystray.MenuItem(lambda *a: state["status"], None, enabled=False),
        # Only shown while it applies, so it reads as an answer to the status
        # line above it rather than a permanent piece of furniture.
        pystray.MenuItem("How to sign in to Claude Code…", do_login_help,
                         visible=lambda item: needs_login()),
        pystray.Menu.SEPARATOR,
        pystray.MenuItem("Feeding", toggle_feeding,
                         checked=lambda item: state["feeding"]),
        # Only worth offering while it is not yet installed. Once it is, the
        # login item is part of the install and "Start at login" below covers
        # turning it on and off.
        pystray.MenuItem("Install on this computer…", do_install,
                         visible=lambda item: not companion.is_installed()),
        pystray.MenuItem("Start at login", toggle_autostart,
                         checked=lambda item: is_autostarted()),
        *_board_items(),
        pystray.MenuItem("Look for boards", do_rescan),
        pystray.Menu.SEPARATOR,
        pystray.MenuItem("Quit", lambda icon, item: icon.stop()),
    )


def _board_items():
    """One board keeps the flat menu it always had. More than one gets a
    submenu each, because every entry below this point — pair, settings,
    firmware — has to say which board it means."""
    boards = state["boards"]
    if len(boards) > 1:
        return [pystray.MenuItem(board_label(b), _board_menu(b["url"]))
                for b in boards]
    return [
        pystray.MenuItem("Pair board (run without this computer)", pair_board()),
        pystray.MenuItem("Disconnect board from Claude", disconnect_board(),
                         visible=lambda item: bool(state["url"])),
        pystray.MenuItem("Open board page", do_open,
                         enabled=lambda item: bool(state["url"])),
        pystray.MenuItem("Settings", pystray.Menu(
            pystray.MenuItem("Screens, clock & auto-rotate…", open_path("/settings")),
            pystray.MenuItem("Phone alerts…", open_path("/alerts")),
            pystray.MenuItem("Update firmware…", open_path("/update")),
        ), enabled=lambda item: bool(state["url"])),
    ]


def main():
    # The tray app has its own entry point and never runs companion.main(), so
    # the stale-auto-start sweep that lives there was never reaching the build
    # that most people actually download. Caught by running the packaged binary
    # rather than the source: the CLI swept, the tray did not.
    #
    # There is no console here to print to, so anything removed has to be said
    # in the UI. It goes in the menu's status line, and is offered as a desktop
    # notification too where the backend supports one.
    companion.sweep_stale_install()
    swept = companion.sweep_stale_autostart()
    if swept:
        state["status"] = ("Removed %d leftover auto-start %s from an older "
                           "version" % (len(swept),
                                        "entry" if len(swept) == 1 else "entries"))
        state["swept"] = swept

    # Adopted without a probe: this runs before the worker thread and a slow
    # network shouldn't hold up the icon appearing. The worker fills in what
    # each one actually is, and rediscovers if none of them answer.
    #
    # "fixed" now means pinned by HEADROOM_PI, not merely remembered. It used
    # to be set for a saved address too, which meant a board that moved or was
    # renamed was never looked for again -- the config stayed truthy and the
    # tray pushed into the void for as long as it ran.
    pinned = initial_urls()
    if pinned:
        state["boards"] = [{"url": u, "board": "board", "id": None}
                           for u in pinned]
        state["url"] = pinned[0]
        state["fixed"] = bool(os.environ.get("HEADROOM_PI", "").strip())
    icon = pystray.Icon("Yoyu", make_icon("amber"), "Yoyu", build_menu())
    threading.Thread(target=worker, args=(icon,), daemon=True).start()
    if state.get("swept"):
        try:
            icon.notify("An older version was still starting a second copy at "
                        "login. Removed it — two of them fight over your Claude "
                        "account and the board stops showing numbers.",
                        "Yoyu")
        except Exception:      # noqa: BLE001 — not every backend has notify()
            pass
    icon.run()   # blocks on the main thread (required on macOS)


if __name__ == "__main__":
    main()
