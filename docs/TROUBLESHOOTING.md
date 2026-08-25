# Troubleshooting

Quick fixes for the most common snags. Find your symptom below.

If you're stuck, most problems come down to one of two things: **the board can't
be reached on the network**, or **the login it's using has gone stale**.

---

## First, which mode are you in?

There are two ways to feed the board its usage. Knowing which you're using
explains most issues:

- **Push mode (recommended).** You run the companion app on your computer and
  leave it running. It reads your existing Claude login and *pushes* the numbers
  to the board. **No login is stored on the board, so nothing on it can expire.**
  This is the mode most people should use.
- **Self-contained / paired mode (advanced).** You ran the companion once with
  `--pair`. The board holds a copy of your Claude login and polls Anthropic
  itself, with no computer running. Powerful, but the login can expire (see
  below), so only use it with a **spare Claude account**.

When in doubt, use **push mode**: just run the companion with no arguments.

---

## The board says "login expired – re-pair"

**What it means:** you're in paired mode, and the login stored on the board
stopped working.

**Why it happens:** you almost certainly paired the board with the **same Claude
account your computer's Claude Code uses**. Claude rotates login tokens on use,
so your computer and the board keep invalidating each other's copy. Within an
hour or so, the board's copy dies.

**Fixes (pick one):**
1. **Switch to push mode (easiest, permanent).** Stop pairing and just run the
   companion normally — it pushes usage and nothing on the board can expire:
   ```
   python companion.py
   ```
   Leave it running (or let it start with your computer).
2. **Pair a spare Claude account.** Use a second/free Claude account for the
   board so your main login never rotates its token out. Then re-pair:
   ```
   python companion.py --pair
   ```

---

## No pairing code appears on the board's screen

**If you're on firmware older than 1.3.0:** update — an early build could repaint
the code off-screen before you saw it. Re-flash from the setup page.

**If you're on 1.3.0+ and still see no code:** the companion is talking to the
**wrong device** (see the 404 section below) or the board isn't on your Wi-Fi
(see "board not found"). The code only appears when the companion successfully
reaches *this* board's `/api/pair/start`.

---

## "Couldn't reach the board … HTTP Error 404"

**What it means:** the companion found *a* device at `yoyu.local`, but it
wasn't your ESP32 board — so the pairing endpoint didn't exist there.

**The usual cause: two devices answering to the same name.** If you also run the
older **Raspberry Pi ClaudeTracker**, both it and the ESP32 Mini advertise
themselves as `yoyu.local`. Your computer resolves whichever answers first,
and if the Pi wins, you get a 404 (it has no pairing endpoint) — and both
devices polling the same Claude account will also **rate-limit** you.

**Fixes:**
1. **Turn off the device you're not using.** If the old Pi is still plugged in
   and you've moved to the Mini, unplug the Pi. Then re-pair.
2. **Point the companion straight at the right board**, bypassing discovery:
   ```
   python companion.py --pi http://<board-ip>:8080 --pair
   ```
   Find the board's IP from your router's device list. To see which device
   `yoyu.local` currently resolves to:
   ```
   ping yoyu.local
   ```

---

## The board shows "rate limited – waiting ~Xm"

**What it means:** Anthropic returned HTTP 429 — too many usage requests in a
short window. The board backs off automatically and recovers on its own.

**Why it happens:** usually **more than one thing polling the same Claude
account** — e.g. the ESP32 *and* an old Pi both self-hosting, or a stray second
companion. Your own heavy Claude use can contribute too.

**Fixes:**
- Make sure only **one** device is self-hosting on a given account (turn off the
  extra one), or move to **push mode** so only the companion talks to Anthropic.
- Then just wait out the countdown — it clears itself.

---

## "Another Yoyu companion is already running"

**What it means:** a companion process is already running on this computer
(often one that started with Windows/macOS login). Only one should run, so the
new one exits to avoid double-polling.

**Fixes:**
- If you *want* the already-running one, do nothing — it's working.
- To run a fresh one instead, stop the existing process first:
  - **Windows (PowerShell):**
    ```powershell
    Get-NetTCPConnection -LocalPort 47823 -ErrorAction SilentlyContinue |
      ForEach-Object { Stop-Process -Id $_.OwningProcess -Force }
    ```
  - **macOS/Linux:**
    ```
    lsof -ti tcp:47823 | xargs kill
    ```
  - Or just reboot the computer.
- To stop it auto-starting with your computer: `python companion.py --uninstall`.

---

## The companion can't find the board at all

**Checklist:**
1. The board and computer are on the **same Wi-Fi** (not a guest network / VPN).
2. The board's screen shows usage or a status screen — not the **"Set me up"**
   Wi-Fi setup screen. If it's in setup mode, reconnect it to Wi-Fi (open the
   setup page and use "Connect to Wi-Fi", or join `Yoyu-Setup` and pick your
   network).
3. Still nothing? Point the companion straight at the board:
   ```
   python companion.py --pi http://<board-ip>:8080
   ```
   Get the IP from your router's device list. Several boards go in as one
   comma-separated list, no spaces.

**If it used to work and stopped**, the saved address has probably gone stale
— a board that moved to a new IP, or a config written before the rename. From
v1.7.0 the companion notices that nothing it has saved answers any more and
looks again by itself; before that it would keep pushing to the old address
forever. `--rescan` forces it.

---

## You have two boards and only one of them updates

Expected on anything before v1.7.0, and fixed in it. The companion used to
stop at the **first** board that answered, so the second was never fed — it
just sat there saying *"waiting for your computer."*

Update the companion, then tell it to look again:

```
python companion.py --rescan
```

From the tray, use **Look for boards**. Nothing rescans on its own once a
saved board answers, which is right until the day you plug in a second one.

Both boards are then kept in the config and fed from a single read of your
usage — the numbers are the same whoever displays them, and polling once per
board would only multiply the rate limiting.

**Two other things change with two boards on one network:**

- **`yoyu.local` only ever names one of them.** The boards notice the
  collision and the second one to start up comes up as `yoyu-2.local`
  instead, so both are reachable — but which is which is decided by boot
  order and swaps when they restart together. Address a specific board by
  its IP, or by the id on its settings page.
- **Pairing is per board.** Each holds its own top-up key, so pairing one
  does nothing for the other. With two boards the tray gives each its own
  submenu, with **Pair this board** inside it. If a board says its key was
  refused, pair it again — that message means the key belonged to the other
  board, or the board was disconnected and issued a new one.

---

## The companion says it can't find your Claude login

The companion reuses the login already on your computer — there's **no separate
sign-in**. It reads the **Claude Code CLI's** login:
- **macOS:** the Keychain item `Claude Code-credentials`
- **Windows/Linux:** `~/.claude/.credentials.json`

> **Being signed into the Claude _desktop app_ or _claude.ai_ in a browser is not
> enough** — the companion specifically needs the **Claude Code CLI** signed in
> on this computer.

Fix: install/sign in to the Claude Code CLI, then try again:
```
claude          # follow the login prompt, or type /login
claude /usage   # should print your real usage
```

### "But Claude Code works fine — I'm definitely signed in"

Then your credentials have most likely been **cleared rather than lost**. The
file is still there and looks normal, but the tokens inside it are empty
strings, so there is nothing for the companion to read. Claude Code itself can
keep working from a token it already holds in memory, which is why the two
disagree.

You can confirm it in a second — the tokens should be long strings, not `""`:

```
python -c "import json,os;o=json.load(open(os.path.expanduser('~/.claude/.credentials.json')))['claudeAiOauth'];print('access',len(o['accessToken']),'refresh',len(o['refreshToken']))"
```

`access 0 refresh 0` means cleared. Signing in again (`claude`, then `/login`)
fixes it.

**What clears them:** most often a second device refreshing the *same* Claude
account. Refresh tokens rotate — when your board refreshes, the copy on your
computer stops being valid, and vice versa. Whichever one refreshes next gets
refused and ends up signed out.

So if this keeps happening every day or two, the two are fighting over one
login. Either give the board its own Claude account, or stop it polling and let
the companion feed it (see the first section).

## The board shows the wrong numbers (e.g. stuck near 100%)

**What it means:** the companion is running, but its output says
`pushed [estimated]:` instead of `pushed [LIVE]:`. It can't read your real Claude
usage, so it's **guessing from local logs** — and the guess is usually wrong
(often pinned near 100%).

**Why:** same root cause as above — the **Claude Code CLI isn't signed in** (or
its token went stale), so there's no real usage to read.

**Fix:**
1. Sign in to the Claude Code CLI: `claude` (then `/login` if prompted). Confirm
   with `claude /usage`.
2. Restart the companion and watch its output — you want **`pushed [LIVE]:`**,
   not `pushed [estimated]:`. Once it says `[LIVE]`, the board matches reality.

---

## Meters are blank / "waiting for usage data"

The board is on Wi-Fi but hasn't received usage yet.
- **Push mode:** make sure the companion is running (see the sections above).
- **Paired mode:** give it a minute to poll; if it stays blank, check for a
  "login expired" or "rate limited" message and follow those sections.

---

Still stuck? Open an issue at
<https://github.com/DaveEuson/Yoyu/issues> with what the board's screen
shows and what the companion prints.
