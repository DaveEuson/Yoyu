# Yoyu よゆう — ESP32-S3 firmware

Yoyu on a **Waveshare ESP32-S3-Touch-LCD-2** (2" ST7789 240×320 IPS,
ESP32-S3R8, battery header). ~$26, no Raspberry Pi, no Linux.

**v0 scope:** the board joins your Wi-Fi (first boot: its own
`Yoyu-Setup` hotspot with a phone setup page, same flow as the Pi), then
speaks the **same HTTP API as the Pi tracker** — `GET /api/status` with the
`"app": "Yoyu"` discovery marker and `POST /api/push` — so the existing
desktop **companion feeds it unchanged**. Claude-night-theme meters, reset
countdowns, NTP clock.

**Reads your usage itself:** run the companion **once** with
`companion.py --pair` (it finds the board automatically) — it hands the board
your existing Claude Code login, no copying by hand, and the board then reads
Anthropic's usage endpoint directly. (Manual fallback if you can't run the
companion: paste the login at `http://<board-ip>:8080/connect`.)

**It keeps going for several hours with your computer off, then waits for it.**
That limit is deliberate. The board is handed a short-lived access token and
cannot renew one for itself: renewing means spending a *rotating* refresh token,
and Claude Code on your computer holds the same one — so a board that renewed
was signing you out of Claude Code roughly once a day. Sharing one Claude
account is the normal case, not a mistake, so the board gets a credential it
cannot break. The companion tops it up whenever it runs, which for most people
means at login without them noticing.

**Prerequisite:** Claude Code installed and signed in on the computer you pair
from. Claude Code comes with the paid Claude plans (Pro and Max), not the free
tier. The board has no API key and no account of its own; it reads Claude
Code's login, so a free Claude account leaves it with nothing to read.

## Easiest: the browser flasher (no tools)

For anyone who just wants it working — no VS Code, no PlatformIO, no git —
open the setup page in **Chrome or Edge on a computer** and click
**Connect & Install**:

> **https://daveeuson.github.io/Yoyu/**

It flashes the latest release straight from the browser, then walks through
Wi-Fi and the companion download. That page is `docs/index.html` here (served
by GitHub Pages), and it flashes the `headroom-mini-merged.bin` that a tagged
release builds. The developer flow below is only needed to change the firmware.

## Developer flow (build from source)

1. Install [VS Code](https://code.visualstudio.com/) + the **PlatformIO IDE**
   extension (or `pip install platformio` for the CLI).
2. Plug the board in over USB-C (**use a data cable**, not a charge-only one).
3. From this `firmware/` folder:

   ```
   pio run -t upload
   pio device monitor        # optional: serial logs at 115200
   ```

   If the upload can't find the port, hold **BOOT**, tap **RESET**, release
   BOOT, retry (classic ESP32 bootloader dance).

4. On the screen: join the `Yoyu-Setup` Wi-Fi from your phone
   (password `yoyusetup`), open `http://192.168.4.1`, then **pick your home
   network from the scanned list** and type its password (or type the name by
   hand if it's hidden). The board reboots and shows its address.
5. Feed it from your computer:

   ```
   python3 companion/companion.py --pi http://<board-ip>:8080 --no-install
   ```

   (`--no-install` so it doesn't fight your Pi tracker's autostart; drop the
   flag if this becomes your only tracker. `yoyu.local` also works if
   your OS resolves mDNS.)

Within a couple of minutes the meters go live.

## Board pinout (for reference)

| Function | GPIO |
|---|---|
| LCD SCLK / MOSI / MISO | 39 / 38 / 40 |
| LCD DC / CS / Backlight | 42 / 45 / 1 |
| LCD RST | — (soft reset) |
| Touch (CST816D, I2C 0x15) | SDA 48 / SCL 47 |

Panel: ST7789, 240×320, IPS, rotation 2 = portrait with the USB-C connector on
top (flip to 0 in `src/main.cpp` if you mount it the other way up).

## Enclosure

A printable case that fits this board well:
https://www.printables.com/model/1188149-enclosure-for-esp32-s3-touch-lcd-2

## Roadmap

- **v0 (this)** — screen + Wi-Fi + companion-fed meters. Bring-up day.
- **Phase 1.5 — touch & motion (done).** CST816D capacitive touch + QMI8658
  IMU on the shared I2C bus (SDA 48 / SCL 47):
  - **Tap / swipe L-R** → cycle screens (meters → focus → history → Yoyu
    → Timer → Actions → Projects → Settings)
  - **Long-press** → toggle % left / % used (saved)
  - **Swipe up / down** → brightness
  - **Face-down** → screen off; **face-up / shake** → wake
  - **BOOT held ~5s** → factory-reset Wi-Fi + login
  - Both sensors degrade gracefully — if a chip isn't found its feature is
    simply off. IMU auto-rotate isn't wired yet (mounting-dependent).
- **Phase 2 — self-contained (done).** Polls Anthropic's usage endpoint
  on-device: paste a login once at `/connect`, token refreshed on-device, no
  companion. TLS currently runs without cert pinning (`setInsecure`) — fine on
  a trusted home network; pin a CA bundle before shipping if that matters.
- **Phase 3 — polish (done).** Battery gauge (VBAT on GPIO5 via the 200K/100K
  divider) shown on every screen; usage history stored in flash with a graph
  screen; and phone push alerts — open `http://<board-ip>/alerts`, enter an
  **ntfy** topic (or Pushover keys) and a threshold, and the board pushes when
  a window crosses it (with a recovery notice). Charge state is inferred from
  voltage (no charge-status line on this board).
- **Phase 4 — countdown & moods (done).** A **Timer** screen showing a live,
  second-by-second `H:MM:SS` countdown to your soonest reset (colored amber
  under 30 min, red under 5). The kitsune reacts to how dire things are — a
  **panic** face (wide eyes, sweat, gasp) under 15% headroom and a **KO** face
  (X eyes, dimmed ears) at zero — and its **tails count your headroom**: three
  above 60% left, two down to 25%, one below that.
- **Phase 5 — hardening (planned, needs hardware).** TLS cert verification
  (embed a CA bundle, drop `setInsecure`) and IMU-driven auto-rotate
  (mounting-dependent, so it needs calibrating on a real board). Both require
  flashing to hardware to validate, so they're tracked for a dedicated release.

## Notes

- `http://<board-ip>:8080/settings` configures the clock timezone (defaults to
  US Eastern; countdowns are timezone-independent), 12/24-hour format, overnight
  dimming, **which screens are in the tap rotation** (meters / focus / history /
  Yoyu / Timer / Actions / Projects / Settings), the **default screen** shown
  at power-on, and optional **auto-rotate** (cycle the enabled screens every
  10-60 s; tapping pauses it).
- Don't know the address? **Tap through to the Settings screen** — it's last in
  the rotation. It prints `ip:port` and the firmware version, and lets you
  toggle the screen rotation without a browser. It refuses to switch *itself*
  off, to switch off the power-on default, or to leave fewer than two screens,
  so the address can't be tapped out of existence; the web form above can still
  do all three.
- Update over Wi-Fi at `http://<board-ip>:8080/update` — it downloads the app
  image from the latest GitHub release into the inactive OTA slot and reboots,
  keeping your Wi-Fi/login/settings. A failed download leaves the running
  firmware untouched; worst case, re-flash over USB. The board checks for a
  newer release ~20 s after boot and every 6 h after that; when one is out it
  shows a small **up-arrow badge** in a screen corner and the landing page's
  Updates button turns into "Update available."
- Touch, battery gauge, and on-device Anthropic polling are not in v0.
- If the saved Wi-Fi can't be reached at boot it falls back to the setup
  hotspot without erasing the saved network (a router reboot won't force
  reprovisioning — power-cycle the board once the router is back).

## Troubleshooting

- **"login expired — re-pair" on the meters screen.** The stored Claude login
  is no longer valid, usually because the token was rotated out. Run the
  **companion app** on your computer once (the tray app, or `companion --pair`)
  and it re-signs the board in automatically; if you set the board up by pasting
  a login at `/connect`, paste a current one there instead. An OTA update never
  touches your login — this is a token issue, not an update regression.
- **It keeps happening every day or two.** The board and your computer's Claude
  Code are almost certainly sharing one login, so they rotate each other's
  refresh token and log each other out. Give the board its **own** Claude login
  and the two stop fighting.
